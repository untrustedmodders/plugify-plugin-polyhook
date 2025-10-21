#include "callback.hpp"

#include <thread>
#include <immintrin.h>

using namespace asmjit;

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
static thread_local std::map<const Callback*, std::array<plg::any, Globals::kMaxFuncArgs + 1>> storage;

struct ArgRegSlot {
	explicit ArgRegSlot(uint32_t idx) {
		argIdx = idx;
		useHighReg = false;
	}

	x86::Reg low;
	x86::Reg high;
	uint32_t argIdx;
	bool useHighReg;
};

bool Callback::hasHiArgSlot(const x86::Compiler& compiler, const TypeId typeId) noexcept {
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

uint64_t Callback::getJitFunc(const FuncSignature& sig, const CallbackEntry pre, const CallbackEntry post) {
	if (m_functionPtr) {
		return m_functionPtr;
	}

	SimpleErrorHandler eh;
	CodeHolder code;
	code.init(rt.environment(), rt.cpuFeatures());
	code.setErrorHandler(&eh);

	// initialize function
	x86::Compiler cc(&code);
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
	x86::Mem argsStack;
	if (stackSize > 0) {
		argsStack = cc.newStack(stackSize, alignment);
	}
	x86::Mem argsStackIdx(argsStack);

	// assigns some register as index reg
	x86::Gp i = cc.newGpz();

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
			cc.mov(argsStackIdx, argSlot.low.as<x86::Gp>());

			if (argSlot.useHighReg) {
				cc.add(i, sizeof(uint32_t));
				offsetNextSlot -= sizeof(uint32_t);

				cc.mov(argsStackIdx, argSlot.high.as<x86::Gp>());
			}
		} else if(TypeUtils::isFloat(argType)) {
			cc.movq(argsStackIdx, argSlot.low.as<x86::Vec>());
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, offsetNextSlot);
		offsetNextSlot = sizeof(uint64_t);
	}

	auto callbackSig = FuncSignature::build<void, Callback*, Parameters*, size_t, Return*, ReturnFlag*>();

	// get pointer to callback and pass it to the user callback
	x86::Gp argCallback = cc.newGpz("argCallback");
	cc.mov(argCallback, this);

	// get pointer to stack structure and pass it to the user callback
	x86::Gp argStruct = cc.newGpz("argStruct");
	cc.lea(argStruct, argsStack);

	// fill reg to pass struct arg count to callback
	x86::Gp argCountParam = cc.newGpz("argCountParam");
	cc.mov(argCountParam, sig.argCount());

	// create buffer for return struct
	x86::Mem retStack = cc.newStack(sizeof(uint64_t), alignment);
	x86::Gp retStruct = cc.newGpz("retStruct");
	cc.lea(retStruct, retStack);

	// create buffer for flag value
	x86::Mem flagStack = cc.newStack(sizeof(ReturnFlag), alignment);
	x86::Gp flagStruct = cc.newGpz("flagStruct");
	cc.lea(flagStruct, flagStack);
	x86::Mem flagStackIdx(flagStack);
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

	x86::Gp flag = cc.newUInt8();
	cc.mov(flag, flagStackIdx);
	cc.test(flag, ReturnFlag::Supercede);
	cc.jnz(supercede);

	// mov from arguments stack structure into regs
	cc.mov(i, 0); // reset idx
	for (const auto& argSlot : argRegSlots) {
		const auto& argType = sig.args()[argSlot.argIdx];

		if (TypeUtils::isInt(argType)) {
			cc.mov(argSlot.low.as<x86::Gp>(), argsStackIdx);

			if (argSlot.useHighReg) {
				cc.add(i, sizeof(uint32_t));
				offsetNextSlot -= sizeof(uint32_t);

				cc.mov(argSlot.high.as<x86::Gp>(), argsStackIdx);
			}
		} else if (TypeUtils::isFloat(argType)) {
			cc.movq(argSlot.low.as<x86::Vec>(), argsStackIdx);
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, offsetNextSlot);
		offsetNextSlot = sizeof(uint64_t);
	}

	// deref the trampoline ptr (holder must live longer, must be concrete reg since push later)
	x86::Gp origPtr = cc.zbx();
	cc.mov(origPtr, (uint64_t) getTrampolineHolder());
	cc.mov(origPtr, ptr(origPtr));

	InvokeNode* origInvokeNode;
	cc.invoke(&origInvokeNode, origPtr, sig);
	for (const auto& argSlot : argRegSlots) {
		origInvokeNode->setArg(argSlot.argIdx, 0, argSlot.low);
		if (argSlot.useHighReg) {
			origInvokeNode->setArg(argSlot.argIdx, 1, argSlot.high);
		}
	}

	if (sig.hasRet()) {
		x86::Reg ret;
		if (TypeUtils::isInt(sig.ret())) {
			ret = cc.newGpz();
		} else {
			ret = cc.newXmm();
		}
		origInvokeNode->setRet(0, ret);

		x86::Mem retStackIdx(retStack);
		retStackIdx.setSize(sizeof(uint64_t));
		if (TypeUtils::isInt(sig.ret())) {
			cc.mov(retStackIdx, ret.as<x86::Gp>());
		} else {
			cc.movq(retStackIdx, ret.as<x86::Vec>());
		}
	}

	// this code will be executed if a callback returns Supercede
	cc.bind(supercede);

	x86::Gp flag2 = cc.newUInt8();
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
			cc.mov(argSlot.low.as<x86::Gp>(), argsStackIdx);

			if (argSlot.useHighReg) {
				cc.add(i, sizeof(uint32_t));
				offsetNextSlot -= sizeof(uint32_t);

				cc.mov(argSlot.high.as<x86::Gp>(), argsStackIdx);
			}
		} else if (TypeUtils::isFloat(argType)) {
			cc.movq(argSlot.low.as<x86::Vec>(), argsStackIdx);
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
		x86::Mem retStackIdx(retStack);
		retStackIdx.setSize(sizeof(uint64_t));
		if (TypeUtils::isInt(sig.ret())) {
			x86::Gp tmp = cc.newGpz();
			cc.mov(tmp, retStackIdx);
			cc.ret(tmp);
		} else {
			x86::Vec tmp = cc.newXmm();
			cc.movq(tmp, retStackIdx);
			cc.ret(tmp);
		}
	}

	cc.func()->frame().addDirtyRegs(origPtr);

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

uint64_t Callback::getJitFunc(const DataType retType, std::span<const DataType> paramTypes, const CallbackEntry pre, const CallbackEntry post, uint8_t vaIndex) {
	FuncSignature sig(CallConvId::kCDecl, vaIndex, getTypeId(retType));
	for (const DataType& type : paramTypes) {
		sig.addArg(getTypeId(type));
	}
	return getJitFunc(sig, pre, post);
}

bool Callback::addCallback(const CallbackType type, const CallbackHandler handler, int priority) {
	if (!handler)
		return false;

	std::unique_lock lock(m_mutex);

	auto& [handlers, priorities] = m_callbacks[static_cast<size_t>(type)];

	if (std::any_of(handlers.begin(), handlers.end(),
		[&](const auto& h){ return h == handler; }))
		return false;

	auto it = std::upper_bound(priorities.begin(), priorities.end(), priority,
		[](int p, int cur){ return p > cur; }); // descending order

	auto index = std::distance(priorities.begin(), it);

	handlers.insert(handlers.begin() + index, handler);
	priorities.insert(priorities.begin() + index, priority);

	return true;
}

bool Callback::removeCallback(const CallbackType type, const CallbackHandler handler) {
	if (!handler)
		return false;

	std::unique_lock lock(m_mutex);

	auto& [handlers, priorities] = m_callbacks[static_cast<size_t>(type)];

    auto it = std::find(handlers.begin(), handlers.end(), handler);
    if (it == handlers.end())
        return false;

    auto index = std::distance(handlers.begin(), it);
    handlers.erase(handlers.begin() + index);
    priorities.erase(priorities.begin() + index);

	return true;
}

bool Callback::isCallbackRegistered(const CallbackType type, const CallbackHandler handler) const noexcept {
	if (!handler)
		return false;

	std::shared_lock lock(m_mutex);

	const auto& [handlers, priorities] = m_callbacks[static_cast<size_t>(type)];

	return std::any_of(handlers.begin(), handlers.end(), [&](const auto& x){ return x == handler; });
}

bool Callback::areCallbacksRegistered(const CallbackType type) const noexcept {
	std::shared_lock lock(m_mutex);

	const auto& [handlers, priorities] = m_callbacks[static_cast<size_t>(type)];

	return !handlers.empty();
}

bool Callback::areCallbacksRegistered() const noexcept {
	return areCallbacksRegistered(CallbackType::Pre) || areCallbacksRegistered(CallbackType::Post);
}

plg::hybrid_vector<Callback::CallbackHandler, Callback::kMaxFuncStack> Callback::getCallbacks(const CallbackType type) noexcept {
	std::shared_lock lock(m_mutex);

	const auto& [handlers, priorities] = m_callbacks[static_cast<size_t>(type)];

	return handlers;
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

DataType Callback::getReturnType() const {
	return m_returnType;
}

DataType Callback::getArgumentType(size_t idx) const {
	return m_arguments[++idx];
}

Callback::Callback(DataType returnType, std::span<const DataType> arguments) : m_returnType(returnType), m_arguments(arguments.begin(), arguments.end()) {
	storage[this] = {};
}

Callback::~Callback() {
	if (m_functionPtr) {
		rt.release(m_functionPtr);
	}
	storage.erase(this);
}