#include "callback.hpp"

#include <thread>
#include <ranges>
#include <immintrin.h>

using namespace asmjit;
using namespace asmjit::x86;

struct SimpleErrorHandler : ErrorHandler {
	Error error{kErrorOk};
	const char* code{};

	void handleError(Error err, const char* message, BaseEmitter*) override {
		error = err;
		code = message;
	}
};

using namespace PLH;

static JitRuntime rt;
static thread_local std::map<Callback const*, std::array<plg::any, Globals::kMaxFuncArgs + 1>> storage;

struct ArgRegSlot {
	explicit ArgRegSlot(uint32_t idx) {
		argIdx = idx;
		useHighReg = false;
	}

	Reg low;
	Reg high;
	uint32_t argIdx;
	bool useHighReg;
};

bool Callback::hasHiArgSlot(const Compiler& compiler, const TypeId typeId) noexcept {
	// 64bit width regs can fit wider args
	if (compiler.is64Bit()) {
		return false;
	}

	switch (typeId) {
		case TypeId::kInt64:
		case TypeId::kUInt64:
			return true;
		default:
			return false;
	}
}

template<typename T>
constexpr TypeId getTypeIdx() noexcept {
	return static_cast<TypeId>(TypeUtils::TypeIdOfT<T>::kTypeId);
}

TypeId Callback::getTypeId(const DataType type) noexcept {
	switch (type) {
		case DataType::Void:
			return getTypeIdx<void>();
		case DataType::Bool:
			return getTypeIdx<bool>();
		case DataType::Int8:
			return getTypeIdx<int8_t>();
		case DataType::Int16:
			return getTypeIdx<int16_t>();
		case DataType::Int32:
			return getTypeIdx<int32_t>();
		case DataType::Int64:
			return getTypeIdx<int64_t>();
		case DataType::UInt8:
			return getTypeIdx<uint8_t>();
		case DataType::UInt16:
			return getTypeIdx<uint16_t>();
		case DataType::UInt32:
			return getTypeIdx<uint32_t>();
		case DataType::UInt64:
			return getTypeIdx<uint64_t>();
		case DataType::Float:
			return getTypeIdx<float>();
		case DataType::Double:
			return getTypeIdx<double>();
		case DataType::Pointer:
		case DataType::String:
			return TypeId::kUIntPtr;
	}
	return TypeId::kVoid;
}

uint64_t Callback::getJitFunc(const FuncSignature& sig, CallbackEntry pre, CallbackEntry post) {
	if (m_functionPtr) {
		return m_functionPtr;
	}

	SimpleErrorHandler eh;
	CodeHolder code;
	code.init(rt.environment(), rt.cpuFeatures());
	code.setErrorHandler(&eh);

	// initialize function
	Compiler cc(&code);
	FuncNode* func = cc.addFunc(sig);

#if 0
	StringLogger log;
	auto kFormatFlags =
			FormatFlags::kMachineCode | FormatFlags::kExplainImms | FormatFlags::kRegCasts
			| FormatFlags::kHexImms | FormatFlags::kHexOffsets  | FormatFlags::kPositions;

	log.addFlags(kFormatFlags);
	code.setLogger(&log);
#endif

#if PLUGIFY_IS_RELEASE
	// too small to really need it
	//func->frame().resetPreservedFP();
#endif

	// Create labels
	Label supercede = cc.newLabel();
	Label noPost = cc.newLabel();

	// map argument slots to registers, following abi.
	std::inplace_vector<ArgRegSlot, Globals::kMaxFuncArgs> argRegSlots;

	for (uint32_t argIdx = 0; argIdx < sig.argCount(); ++argIdx) {
		const auto& argType = sig.args()[argIdx];

		ArgRegSlot argSlot(argIdx);

		if (TypeUtils::isInt(argType)) {
			argSlot.low = cc.newGpz();

			if (hasHiArgSlot(cc, argType)) {
				argSlot.high = cc.newGpz();
				argSlot.useHighReg = true;
			}
		} else if (TypeUtils::isFloat(argType)) {
			argSlot.low = cc.newXmm();
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		func->setArg(argSlot.argIdx, 0, argSlot.low);
		if (argSlot.useHighReg) {
			func->setArg(argSlot.argIdx, 1, argSlot.high);
		}

		argRegSlots.push_back(std::move(argSlot));
	}

	const uint32_t alignment = 16;
	uint32_t offsetNextSlot = sizeof(uint64_t);

	// setup the stack structure to hold arguments for user callback
	const auto stackSize = static_cast<uint32_t>(sizeof(uint64_t) * sig.argCount());
	Mem argsStack;
	if (stackSize > 0) {
		argsStack = cc.newStack(stackSize, alignment);
	}
	Mem argsStackIdx(argsStack);

	// assigns some register as index reg
	Gp i = cc.newGpz();

	// stackIdx <- stack[i].
	argsStackIdx.setIndex(i);

	// r/w are sizeof(uint64_t) width now
	argsStackIdx.setSize(sizeof(uint64_t));

	// set i = 0
	cc.mov(i, 0);
	//// mov from arguments registers into the stack structure
	for (const auto& argSlot : argRegSlots) {
		const auto& argType = sig.args()[argSlot.argIdx];

		// have to cast back to explicit register types to gen right mov type
		if (TypeUtils::isInt(argType)) {
			cc.mov(argsStackIdx, argSlot.low.as<Gp>());

			if (argSlot.useHighReg) {
				cc.add(i, sizeof(uint32_t));
				offsetNextSlot -= sizeof(uint32_t);

				cc.mov(argsStackIdx, argSlot.high.as<Gp>());
			}
		} else if(TypeUtils::isFloat(argType)) {
			cc.movq(argsStackIdx, argSlot.low.as<Vec>());
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, offsetNextSlot);
		offsetNextSlot = sizeof(uint64_t);
	}

	auto callbackSig = FuncSignature::build<void, Callback*, uint64_t*, size_t, void*, ReturnFlag*>();

	// get pointer to callback and pass it to the user callback
	Gp argCallback = cc.newGpz("argCallback");
	cc.mov(argCallback, this);

	// get pointer to stack structure and pass it to the user callback
	Gp argStruct = cc.newGpz("argStruct");
	cc.lea(argStruct, argsStack);

	// fill reg to pass struct arg count to callback
	Gp argCountParam = cc.newGpz("argCountParam");
	cc.mov(argCountParam, sig.argCount());

	// create buffer for return struct
	Mem retStack = cc.newStack(sizeof(uint64_t), alignment);
	Gp retStruct = cc.newGpz("retStruct");
	cc.lea(retStruct, retStack);

	// create buffer for flag value
	Mem flagStack = cc.newStack(sizeof(ReturnFlag), alignment);
	Gp flagStruct = cc.newGpz("flagStruct");
	cc.lea(flagStruct, flagStack);
	Mem flagStackIdx(flagStack);
	flagStackIdx.setSize(sizeof(ReturnFlag));
	cc.mov(flagStackIdx, ReturnFlag::Default);

	InvokeNode* invokePreNode;

	// Call pre callback
	cc.invoke(&invokePreNode, (uint64_t)pre, callbackSig);

	// call to user provided function (use ABI of host compiler)
	invokePreNode->setArg(0, argCallback);
	invokePreNode->setArg(1, argStruct);
	invokePreNode->setArg(2, argCountParam);
	invokePreNode->setArg(3, retStruct);
	invokePreNode->setArg(4, flagStruct);

	Gp flag = cc.newUInt8();
	cc.mov(flag, flagStackIdx);
	cc.test(flag, ReturnFlag::Supercede);
	cc.jnz(supercede);

	// mov from arguments stack structure into regs
	cc.mov(i, 0); // reset idx
	for (const auto& argSlot : argRegSlots) {
		const auto& argType = sig.args()[argSlot.argIdx];

		if (TypeUtils::isInt(argType)) {
			cc.mov(argSlot.low.as<Gp>(), argsStackIdx);

			if (argSlot.useHighReg) {
				cc.add(i, sizeof(uint32_t));
				offsetNextSlot -= sizeof(uint32_t);

				cc.mov(argSlot.high.as<Gp>(), argsStackIdx);
			}
		} else if (TypeUtils::isFloat(argType)) {
			cc.movq(argSlot.low.as<Vec>(), argsStackIdx);
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, offsetNextSlot);
		offsetNextSlot = sizeof(uint64_t);
	}

	cc.mov(i, ptr((uint64_t)getTrampolineHolder()));

	InvokeNode* origInvokeNode;
	cc.invoke(&origInvokeNode, i, sig);
	for (const auto& argSlot : argRegSlots) {
		origInvokeNode->setArg(argSlot.argIdx, 0, argSlot.low);
		if (argSlot.useHighReg) {
			origInvokeNode->setArg(argSlot.argIdx, 1, argSlot.high);
		}
	}

	if (sig.hasRet()) {
		Reg ret;
		if (TypeUtils::isInt(sig.ret())) {
			ret = cc.newGpz();
		} else {
			ret = cc.newXmm();
		}
		origInvokeNode->setRet(0, ret);

		Mem retStackIdx(retStack);
		retStackIdx.setSize(sizeof(uint64_t));
		if (TypeUtils::isInt(sig.ret())) {
			cc.mov(retStackIdx, ret.as<Gp>());
		} else {
			cc.movq(retStackIdx, ret.as<Vec>());
		}
	}

	// this code will be executed if a callback returns Supercede
	cc.bind(supercede);

	Gp flag2 = cc.newUInt8();
	cc.mov(flag2, flagStackIdx);
	cc.test(flag2, ReturnFlag::NoPost);
	cc.jnz(noPost);

	InvokeNode* invokePostNode;

	// Call post callback
	cc.invoke(&invokePostNode, (uint64_t)post, callbackSig);

	// call to user provided function (use ABI of host compiler)
	invokePostNode->setArg(0, argCallback);
	invokePostNode->setArg(1, argStruct);
	invokePostNode->setArg(2, argCountParam);
	invokePostNode->setArg(3, retStruct);
	invokePostNode->setArg(4, flagStruct);

	// mov from arguments stack structure into regs
	cc.mov(i, 0); // reset idx
	for (const auto& argSlot : argRegSlots) {
		const auto& argType = sig.args()[argSlot.argIdx];

		if (TypeUtils::isInt(argType)) {
			cc.mov(argSlot.low.as<Gp>(), argsStackIdx);

			if (argSlot.useHighReg) {
				cc.add(i, sizeof(uint32_t));
				offsetNextSlot -= sizeof(uint32_t);

				cc.mov(argSlot.high.as<Gp>(), argsStackIdx);
			}
		} else if (TypeUtils::isFloat(argType)) {
			cc.movq(argSlot.low.as<Vec>(), argsStackIdx);
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, offsetNextSlot);
		offsetNextSlot = sizeof(uint64_t);
	}

	cc.bind(noPost);

	if (sig.hasRet()) {
		Mem retStackIdx(retStack);
		retStackIdx.setSize(sizeof(uint64_t));
		if (TypeUtils::isInt(sig.ret())) {
			Gp tmp = cc.newGpz();
			cc.mov(tmp, retStackIdx);
			cc.ret(tmp);
		} else {
			Vec tmp = cc.newXmm();
			cc.movq(tmp, retStackIdx);
			cc.ret(tmp);
		}
	}

	cc.endFunc();

	cc.finalize();

	rt.add(&m_functionPtr, &code);

	if (eh.error) {
		m_functionPtr = 0;
		m_errorCode = eh.code;
		return 0;
	}

#if 0
	Log::log("JIT Stub:\n" + std::string(log.data()), ErrorLevel::INFO);
#endif

	return m_functionPtr;
}

uint64_t Callback::getJitFunc(const Signature& sig, CallbackEntry pre, CallbackEntry post) {
	FuncSignature signature(CallConvId::kCDecl, static_cast<uint32_t>(sig.varIndex), getTypeId(sig.returnType));
	for (const DataType& type : sig.arguments) {
		signature.addArg(getTypeId(type));
	}
	return getJitFunc(signature, pre, post);
}

uint64_t Callback::getJitFunc(CallbackEntry2 entry) {
	if (m_functionPtr) {
		return m_functionPtr;
	}

	SimpleErrorHandler eh;
	CodeHolder code;
	code.init(rt.environment(), rt.cpuFeatures());
	code.setErrorHandler(&eh);

	// initialize function
	Assembler a(&code);

#if 0
	StringLogger log;
	auto kFormatFlags =
			FormatFlags::kMachineCode | FormatFlags::kExplainImms | FormatFlags::kRegCasts
			| FormatFlags::kHexImms | FormatFlags::kHexOffsets  | FormatFlags::kPositions;

	log.addFlags(kFormatFlags);
	code.setLogger(&log);
#endif

	// save scratch registers that are used by setReturnAddress
	saveRegisters(a);

#ifdef POLYHOOK2_ARCH_X64
#ifdef POLYHOOK2_OS_WINDOWS
	a.lea(rdx, ptr(rsp));
	a.mov(rcx, this);
	a.mov(rbx, rsp);
	a.and_(rsp, -16);
	a.sub(rsp, 32);
	a.call(entry);
	a.mov(rsp, rbx);
#else // __systemV__
	a.lea(rsi, ptr(rsp));
	a.mov(rdi, this);
	a.mov(rbx, rsp);
	a.and_(rsp, -16);
	a.call(entry);
	a.mov(rsp, rbx);
#endif
#else
	a.lea(eax, ptr(esp));
	a.mov(ebx, esp);
	a.and_(esp, -16);
	a.push(eax);
	a.push(this);
	a.call(entry);
	a.mov(esp, ebx);
#endif

	restoreRegisters(a);

#ifdef POLYHOOK2_ARCH_X64
	a.push(rax);
	a.mov(rax, ptr((uint64_t)getTrampolineHolder()));
	a.xchg(ptr(rsp), rax);
#else
	a.push(eax);
	a.mov(eax, ptr((uint64_t)getTrampolineHolder()));
	a.xchg(ptr(esp), eax);
#endif

	a.ret();

	a.finalize();

	rt.add(&m_functionPtr, &code);

	if (eh.error) {
		m_functionPtr = 0;
		m_errorCode = eh.code;
		return 0;
	}

#if 0
	Log::log("JIT Stub:\n" + std::string(log.data()), ErrorLevel::INFO);
#endif

	return m_functionPtr;
}

bool Callback::addCallback(CallbackType type, CallbackHandler handler, int priority) {
	if (!handler)
		return false;

	std::unique_lock lock(m_mutex);

	auto old = m_callbacks[static_cast<size_t>(type)];
	if (old && std::ranges::any_of(old->handlers, [&](auto h){ return h == handler; }))
		return false;

	auto fresh = std::make_shared<CallbackObject>(old ? *old : CallbackObject{});
	auto it = std::ranges::upper_bound(fresh->priorities.begin(), fresh->priorities.end(), priority,
		[](int p, int cur){ return p > cur; });
	auto idx = std::distance(fresh->priorities.begin(), it);
	fresh->handlers.insert(fresh->handlers.begin() + idx, handler);
	fresh->priorities.insert(fresh->priorities.begin() + idx, priority);

	m_callbacks[static_cast<size_t>(type)] = std::move(fresh);
}

bool Callback::removeCallback(CallbackType type, CallbackHandler handler) {
	if (!handler)
		return false;

	std::unique_lock lock(m_mutex);

	auto old = m_callbacks[static_cast<size_t>(type)];
	if (!old)
		return false;

	auto it = std::ranges::find(old->handlers, handler);
	if (it == old->handlers.end())
		return false;

	auto index = std::distance(old->handlers.begin(), it);

	auto fresh = std::make_shared<CallbackObject>(*old);
	fresh->handlers.erase(fresh->handlers.begin() + index);
	fresh->priorities.erase(fresh->priorities.begin() + index);

	m_callbacks[static_cast<size_t>(type)] = std::move(fresh);
	return true;
}

bool Callback::isCallbackRegistered(CallbackType type, CallbackHandler handler) const noexcept {
	if (!handler)
		return false;

	std::shared_lock lock(m_mutex);
	auto snapshot = m_callbacks[static_cast<size_t>(type)];
	return snapshot && std::ranges::any_of(snapshot->handlers, [&](const auto& h){ return h == handler; });
}

bool Callback::areCallbacksRegistered(CallbackType type) const noexcept {
	std::shared_lock lock(m_mutex);
	auto snapshot = m_callbacks[static_cast<size_t>(type)];
	return snapshot && !snapshot->handlers.empty();
}

bool Callback::areCallbacksRegistered() const noexcept {
	return areCallbacksRegistered(CallbackType::Pre) || areCallbacksRegistered(CallbackType::Post);
}

std::shared_ptr<const Callback::CallbackObject> Callback::getCallbackObject(CallbackType type) const noexcept {
	std::shared_lock lock(m_mutex);
	return m_callbacks[static_cast<size_t>(type)];
}

uint64_t* Callback::getTrampolineHolder() noexcept {
	return &m_trampolinePtr;
}

uint64_t* Callback::getFunctionHolder() noexcept {
	return &m_functionPtr;
}

std::string_view Callback::getError() const noexcept {
	return !m_functionPtr && m_errorCode ? m_errorCode : "";
}

plg::any& Callback::setStorage(size_t idx, const plg::any& any) const {
	return storage[this][++idx] = any;
}

plg::any& Callback::getStorage(size_t idx) const {
	return storage[this][++idx];
}

DataType Callback::getReturnType() const noexcept {
	return m_returnType;
}

DataType Callback::getArgumentType(size_t idx) const noexcept {
	return m_arguments[++idx];
}

std::string_view Callback::getDebugName(std::optional<CallbackType> type) const noexcept {
	return type ? m_names[static_cast<size_t>(*type)] : m_name;
}

Callback::Callback(const Signature& sig) : m_name(sig.name), m_returnType(sig.returnType), m_arguments(sig.arguments.begin(), sig.arguments.end()) {
	if (!m_name.empty()) {
		m_names[static_cast<size_t>(CallbackType::Pre)] = m_name + "::Pre";
		m_names[static_cast<size_t>(CallbackType::Post)] = m_name + "::Post";
	}
	storage[this] = {};
}

Callback::~Callback() {
	if (m_functionPtr) {
		rt.release(m_functionPtr);
	}
	storage.erase(this);
}

void Callback::saveRegisters(Assembler& a) {
#ifdef POLYHOOK2_ARCH_X64
	constexpr int n = 16;

	a.pushfq();

	for (int i = 0; i < n; ++i) {
		if (i == Gp::kIdSp) continue;
		a.push(gpq(i));
	}

	a.sub(rsp, 16 * n);
	for (int i = n - 1; i >= 0; --i) {
		a.movdqu(xmmword_ptr(rsp, 16 * i), xmm(i));
	}
#else
	constexpr int n = 8;

	a.pushfd();

	for (int i = 0; i < n; ++i) {
		if (i == Gp::kIdSp) continue;
		a.push(gpd(i));
	}

	a.sub(esp, 16 * n);
	for (int i = n - 1; i >= 0; --i) {
		a.movdqu(xmmword_ptr(esp, 16 * i), xmm(i));
	}

	//a.sub(esp, 16);
	//a.fstp(tword_ptr(esp));
#endif
}

void Callback::restoreRegisters(Assembler& a) {
#ifdef POLYHOOK2_ARCH_X64
	constexpr int n = 16;

	for (int i = 0; i < n; ++i) {
		a.movdqu(xmm(i), xmmword_ptr(rsp, 16 * i));
	}
	a.add(rsp, 16 * n);

	for (int i = n - 1; i >= 0; --i) {
		if (i == Gp::kIdSp) continue;
		a.pop(gpq(i));
	}

	a.popfq();

#else
	constexpr int n = 8;

	//a.fld(tword_ptr(esp));
	//a.add(esp, 16);

	for (int i = 0; i < n; ++i) {
		a.movdqu(xmm(i), xmmword_ptr(esp, 16 * i));
	}
	a.add(esp, 16 * n);

	for (int i = n - 1; i >= 0; --i) {
		if (i == Gp::kIdSp) continue;
		a.pop(gpd(i));
	}

	a.popfd();

#endif
}
