#include "callback.hpp"

#include <thread>
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

	auto callbackSig = FuncSignature::build<void, Callback*, Parameters*, size_t, Return*, ReturnFlag*>();

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

uint64_t Callback::getJitFunc(const DataType retType, std::span<const DataType> paramTypes, CallbackEntry pre, CallbackEntry post, uint8_t vaIndex) {
	FuncSignature sig(CallConvId::kCDecl, vaIndex, getTypeId(retType));
	for (const DataType& type : paramTypes) {
		sig.addArg(getTypeId(type));
	}
	return getJitFunc(sig, pre, post);
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
	int32_t pushed = saveRegisters(a);

#ifdef POLYHOOK2_ARCH_X64
#ifdef POLYHOOK2_OS_WINDOWS
	a.lea(r8, qword_ptr(rsp, pushed));
	a.mov(rdx, qword_ptr(rsp, pushed));
	a.mov(rcx, this);
	a.mov(rbx, rsp);
	a.and_(rsp, -16);
	a.sub(rsp, 40);
	a.call(entry);
	a.mov(rsp, rbx);
#else // __systemV__
	a.lea(rdx, qword_ptr(rsp, pushed));
	a.mov(rsi, qword_ptr(rsp, pushed));
	a.mov(rdi, this);
	a.mov(rbx, rsp);
	a.and_(rsp, -16);
	a.sub(rsp, 24);
	a.call(entry);
	a.mov(rsp, rbx);
#endif
#else
	a.lea(eax, dword_ptr(esp, pushed));
	a.mov(ecx, dword_ptr(esp, pushed));
	a.mov(ebx, esp);
	a.and_(esp, -16);
	a.push(eax);
	a.push(ecx);
	a.push(this);
	a.call(entry);
	a.mov(esp, ebx);
#endif

	restoreRegisters(a);

#ifdef POLYHOOK2_ARCH_X64
	a.push(rax);
	a.mov(rax, ptr((uint64_t)getTrampolineHolder()));
	a.xchg(qword_ptr(rsp), rax);
#else
	a.push(eax);
	a.mov(eax, ptr((uint64_t)getTrampolineHolder()));
	a.xchg(dword_ptr(esp), eax);
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

bool Callback::removeCallback(CallbackType type, CallbackHandler handler) {
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

bool Callback::isCallbackRegistered(CallbackType type, CallbackHandler handler) const noexcept {
	if (!handler)
		return false;

	std::shared_lock lock(m_mutex);

	const auto& [handlers, priorities] = m_callbacks[static_cast<size_t>(type)];

	return std::any_of(handlers.begin(), handlers.end(), [&](const auto& x){ return x == handler; });
}

bool Callback::areCallbacksRegistered(CallbackType type) const noexcept {
	std::shared_lock lock(m_mutex);

	const auto& [handlers, priorities] = m_callbacks[static_cast<size_t>(type)];

	return !handlers.empty();
}

bool Callback::areCallbacksRegistered() const noexcept {
	return areCallbacksRegistered(CallbackType::Pre) || areCallbacksRegistered(CallbackType::Post);
}

plg::hybrid_vector<Callback::CallbackHandler, Callback::kMaxFuncStack> Callback::getCallbacks(CallbackType type) noexcept {
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

Callback::Callback() {
	storage[this] = {};
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

int32_t Callback::saveRegisters(Assembler& a) {
#ifdef POLYHOOK2_ARCH_X64
	constexpr int n = 16;

	a.pushfq();

	for (int i = 0; i < n; ++i) {
		if (i == Gp::kIdSp) continue;
		a.push(gpq(i));
	}

	a.sub(rsp, 16 * n);
	for (int i = 0; i < n; ++i) {
		a.movdqu(xmmword_ptr(rsp, 16 * i), xmm(i));
	}

	return (n * 8) + (n * 16);
#else
	constexpr int n = 8;

	a.pushfd();

	for (int i = 0; i < n; ++i) {
		if (i == Gp::kIdSp) continue;
		a.push(gpd(i));
	}

	a.sub(esp, 16 * n);
	for (int i = 0; i < n; ++i) {
		a.movdqu(xmmword_ptr(esp, 16 * i), xmm(i));
	}

	a.sub(esp, 16);
	a.fstp(tword_ptr(esp));

	return (n * 4) + (n * 16) + 16;
#endif
}

void Callback::restoreRegisters(Assembler& a) {
#ifdef POLYHOOK2_ARCH_X64
	constexpr int n = 16;

	for (int i = n - 1; i >= 0; --i) {
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

	a.fld(tword_ptr(esp));
	a.add(esp, 16);

	for (int i = n - 1; i >= 0; --i) {
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

#if 0
enum class RegisterType : uint8_t {
	// no register at all.
	NONE,

	// ========================================================================
	// >> 8-bit General purpose registers
	// ========================================================================
	AL,
	CL,
	DL,
	BL,

#ifdef POLYHOOK2_ARCH_X64
	SPL,
	BPL,
	SIL,
	DIL,
	R8B,
	R9B,
	R10B,
	R11B,
	R12B,
	R13B,
	R14B,
	R15B,
#endif // POLYHOOK2_ARCH_X64

	AH,
	CH,
	DH,
	BH,

	// ========================================================================
	// >> 16-bit General purpose registers
	// ========================================================================
	AX,
	CX,
	DX,
	BX,
	SP,
	BP,
	SI,
	DI,

#ifdef POLYHOOK2_ARCH_X64
	R8W,
	R9W,
	R10W,
	R11W,
	R12W,
	R13W,
	R14W,
	R15W,
#endif // POLYHOOK2_ARCH_X64

	// ========================================================================
	// >> 32-bit General purpose registers
	// ========================================================================
	EAX,
	ECX,
	EDX,
	EBX,
	ESP,
	EBP,
	ESI,
	EDI,

#ifdef POLYHOOK2_ARCH_X64
	R8D,
	R9D,
	R10D,
	R11D,
	R12D,
	R13D,
	R14D,
	R15D,
#endif // POLYHOOK2_ARCH_X64

	// ========================================================================
	// >> 64-bit General purpose registers
	// ========================================================================
#ifdef POLYHOOK2_ARCH_X64
	RAX,
	RCX,
	RDX,
	RBX,
	RSP,
	RBP,
	RSI,
	RDI,

	R8,
	R9,
	R10,
	R11,
	R12,
	R13,
	R14,
	R15,
#endif // POLYHOOK2_ARCH_X64

	// ========================================================================
	// >> 64-bit MM (MMX) registers
	// ========================================================================
	MM0,
	MM1,
	MM2,
	MM3,
	MM4,
	MM5,
	MM6,
	MM7,

	// ========================================================================
	// >> 128-bit XMM registers
	// ========================================================================
	XMM0,
	XMM1,
	XMM2,
	XMM3,
	XMM4,
	XMM5,
	XMM6,
	XMM7,
#ifdef POLYHOOK2_ARCH_X64
	XMM8,
	XMM9,
	XMM10,
	XMM11,
	XMM12,
	XMM13,
	XMM14,
	XMM15,
#ifdef POLYHOOK2_PLATFORM_AVX512
	XMM16,
	XMM17,
	XMM18,
	XMM19,
	XMM20,
	XMM21,
	XMM22,
	XMM23,
	XMM24,
	XMM25,
	XMM26,
	XMM27,
	XMM28,
	XMM29,
	XMM30,
	XMM31,
#endif // POLYHOOK2_PLATFORM_AVX512
#endif // POLYHOOK2_ARCH_X64

	// ========================================================================
	// >> 256-bit YMM registers
	// ========================================================================
#ifdef POLYHOOK2_ARCH_X64
#ifdef POLYHOOK2_PLATFORM_AVX
	YMM0,
	YMM1,
	YMM2,
	YMM3,
	YMM4,
	YMM5,
	YMM6,
	YMM7,
	YMM8,
	YMM9,
	YMM10,
	YMM11,
	YMM12,
	YMM13,
	YMM14,
	YMM15,
#ifdef POLYHOOK2_PLATFORM_AVX512
	YMM16,
	YMM17,
	YMM18,
	YMM19,
	YMM20,
	YMM21,
	YMM22,
	YMM23,
	YMM24,
	YMM25,
	YMM26,
	YMM27,
	YMM28,
	YMM29,
	YMM30,
	YMM31,
#endif // POLYHOOK2_PLATFORM_AVX512
#endif // POLYHOOK2_PLATFORM_AVX
#endif // POLYHOOK2_ARCH_X64

	// ========================================================================
	// >> 512-bit ZMM registers
	// ========================================================================
#ifdef POLYHOOK2_PLATFORM_AVX512
	ZMM0,
	ZMM1,
	ZMM2,
	ZMM3,
	ZMM4,
	ZMM5,
	ZMM6,
	ZMM7,
	ZMM8,
	ZMM9,
	ZMM10,
	ZMM11,
	ZMM12,
	ZMM13,
	ZMM14,
	ZMM15,
	ZMM16,
	ZMM17,
	ZMM18,
	ZMM19,
	ZMM20,
	ZMM21,
	ZMM22,
	ZMM23,
	ZMM24,
	ZMM25,
	ZMM26,
	ZMM27,
	ZMM28,
	ZMM29,
	ZMM30,
	ZMM31,
#endif // POLYHOOK2_PLATFORM_AVX512

	// ========================================================================
	// >> 16-bit Segment registers
	// ========================================================================
	CS,
	SS,
	DS,
	ES,
	FS,
	GS,

	// ========================================================================
	// >> 80-bit FPU registers
	// ========================================================================
#ifdef POLYHOOK2_ARCH_X86
	ST0,
	ST1,
	ST2,
	ST3,
	ST4,
	ST5,
	ST6,
	ST7,
#endif // POLYHOOK2_ARCH_X86

	// ========================================================================
	// >> EFLAGS
	// ========================================================================
	EF,

	// ========================================================================
	// >> Maximum value of this enum.
	// ========================================================================
	REG_COUNT
};
using enum RegisterType;

static std::array s_registers = {
	EF,
#ifdef POLYHOOK2_ARCH_X64
	RSP,
	RBP,
	RAX,
	RBX,
	RCX,
	RDX,
	RSI,
	RDI,
	R8,
	R9,
	R10,
	R11,
	R12,
	R13,
	R14,
	R15,
	XMM0,
	XMM1,
	XMM2,
	XMM3,
	XMM4,
	XMM5,
	XMM6,
	XMM7,
	XMM8,
	XMM9,
	XMM10,
	XMM11,
	XMM12,
	XMM13,
	XMM14,
	XMM15,
#else
	ESP,
	EBP,
	EAX,
	EBX,
	ECX,
	EDX,
	ESI,
	EDI,
	XMM0,
	XMM1,
	XMM2,
	XMM3,
	XMM4,
	XMM5,
	XMM6,
	XMM7,
#endif //  POLYHOOK2_ARCH_X64
};

#ifdef POLYHOOK2_ARCH_X64
#define xsp rsp
#else
#define xsp esp
#endif

int32_t Callback::saveScratchRegisters(Assembler& a) {
	int32_t size = 0;
	for (const RegisterType& reg : s_registers) {
		constexpr size_t p = sizeof(void*);
		switch (reg) {
			// ========================================================================
			// >> 8-bit General purpose registers
			// ========================================================================
			case AL: a.push(al); size += p; break;
			case CL: a.push(cl); size += p; break;
			case DL: a.push(dl); size += p; break;
			case BL: a.push(bl); size += p; break;
#ifdef POLYHOOK2_ARCH_X64
			case SPL: a.push(spl); size += p; break;
			case BPL: a.push(bpl); size += p; break;
			case SIL: a.push(sil); size += p; break;
			case DIL: a.push(dil); size += p; break;
			case R8B: a.push(r8b); size += p; break;
			case R9B: a.push(r9b); size += p; break;
			case R10B: a.push(r10b); size += p; break;
			case R11B: a.push(r11b); size += p; break;
			case R12B: a.push(r12b); size += p; break;
			case R13B: a.push(r13b); size += p; break;
			case R14B: a.push(r14b); size += p; break;
			case R15B: a.push(r15b); size += p; break;
#endif
			case AH: a.push(ah); size += p; break;
			case CH: a.push(ch); size += p; break;
			case DH: a.push(dh); size += p; break;
			case BH: a.push(bh); size += p; break;

			// ========================================================================
			// >> 16-bit General purpose registers
			// ========================================================================
			case AX: a.push(ax); size += p; break;
			case CX: a.push(cx); size += p; break;
			case DX: a.push(dx); size += p; break;
			case BX: a.push(bx); size += p; break;
			case SP: a.push(sp); size += p; break;
			case BP: a.push(bp); size += p; break;
			case SI: a.push(si); size += p; break;
			case DI: a.push(di); size += p; break;
#ifdef POLYHOOK2_ARCH_X64
			case R8W: a.push(r8w); size += p; break;
			case R9W: a.push(r9w); size += p; break;
			case R10W: a.push(r10w); size += p; break;
			case R11W: a.push(r11w); size += p; break;
			case R12W: a.push(r12w); size += p; break;
			case R13W: a.push(r13w); size += p; break;
			case R14W: a.push(r14w); size += p; break;
			case R15W: a.push(r15w); size += p; break;
#endif

			// ========================================================================
			// >> 32-bit General purpose registers
			// ========================================================================
			case EAX: a.push(eax); size += p; break;
			case ECX: a.push(ecx); size += p; break;
			case EDX: a.push(edx); size += p; break;
			case EBX: a.push(ebx); size += p; break;
			case ESP: a.push(esp); size += p; break;
			case EBP: a.push(ebp); size += p; break;
			case ESI: a.push(esi); size += p; break;
			case EDI: a.push(edi); size += p; break;
#ifdef POLYHOOK2_ARCH_X64
			case R8D: a.push(r8d); size += p; break;
			case R9D: a.push(r9d); size += p; break;
			case R10D: a.push(r10d); size += p; break;
			case R11D: a.push(r11d); size += p; break;
			case R12D: a.push(r12d); size += p; break;
			case R13D: a.push(r13d); size += p; break;
			case R14D: a.push(r14d); size += p; break;
			case R15D: a.push(r15d); size += p; break;
#endif

			// ========================================================================
			// >> 64-bit General purpose registers
			// ========================================================================
#ifdef POLYHOOK2_ARCH_X64
			case RAX: a.push(rax); size += p; break;
			case RCX: a.push(rcx); size += p; break;
			case RDX: a.push(rdx); size += p; break;
			case RBX: a.push(rbx); size += p; break;
			case RSP: a.push(rsp); size += p; break;
			case RBP: a.push(rbp); size += p; break;
			case RSI: a.push(rsi); size += p; break;
			case RDI: a.push(rdi); size += p; break;

			case R8: a.push(r8); size += p; break;
			case R9: a.push(r9); size += p; break;
			case R10: a.push(r10); size += p; break;
			case R11: a.push(r11); size += p; break;
			case R12: a.push(r12); size += p; break;
			case R13: a.push(r13); size += p; break;
			case R14: a.push(r14); size += p; break;
			case R15: a.push(r15); size += p; break;
#endif

			// ========================================================================
			// >> 64-bit MM (MMX) registers
			// ========================================================================
			case MM0: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm0); size += 8; break;
			case MM1: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm1); size += 8; break;
			case MM2: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm2); size += 8; break;
			case MM3: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm3); size += 8; break;
			case MM4: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm4); size += 8; break;
			case MM5: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm5); size += 8; break;
			case MM6: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm6); size += 8; break;
			case MM7: a.sub(xsp, 8); a.movq(qword_ptr(xsp), mm7); size += 8; break;

			// ========================================================================
			// >> 128-bit XMM registers
			// ========================================================================
			case XMM0: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm0); size += 16; break;
			case XMM1: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm1); size += 16; break;
			case XMM2: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm2); size += 16; break;
			case XMM3: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm3); size += 16; break;
			case XMM4: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm4); size += 16; break;
			case XMM5: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm5); size += 16; break;
			case XMM6: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm6); size += 16; break;
			case XMM7: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm7); size += 16; break;
#ifdef POLYHOOK2_ARCH_X64
			case XMM8: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm8); size += 16; break;
			case XMM9: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm9); size += 16; break;
			case XMM10: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm10); size += 16; break;
			case XMM11: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm11); size += 16; break;
			case XMM12: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm12); size += 16; break;
			case XMM13: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm13); size += 16; break;
			case XMM14: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm14); size += 16; break;
			case XMM15: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm15); size += 16; break;
#endif
#ifdef POLYHOOK2_ARCH_AVX512
			case XMM16: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm16); size += 16; break;
			case XMM17: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm17); size += 16; break;
			case XMM18: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm18); size += 16; break;
			case XMM19: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm19); size += 16; break;
			case XMM20: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm20); size += 16; break;
			case XMM21: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm21); size += 16; break;
			case XMM22: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm22); size += 16; break;
			case XMM23: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm23); size += 16; break;
			case XMM24: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm24); size += 16; break;
			case XMM25: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm25); size += 16; break;
			case XMM26: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm26); size += 16; break;
			case XMM27: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm27); size += 16; break;
			case XMM28: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm28); size += 16; break;
			case XMM29: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm29); size += 16; break;
			case XMM30: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm30); size += 16; break;
			case XMM31: a.sub(xsp, 16); a.movaps(xmmword_ptr(xsp), xmm31); size += 16; break;
#endif // POLYHOOK2_ARCH_AVX512

			// ========================================================================
			// >> 256-bit YMM registers
			// ========================================================================
#ifdef POLYHOOK2_ARCH_AVX
			case YMM0: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm0); size += 32; break;
			case YMM1: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm1); size += 32; break;
			case YMM2: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm2); size += 32; break;
			case YMM3: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm3); size += 32; break;
			case YMM4: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm4); size += 32; break;
			case YMM5: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm5); size += 32; break;
			case YMM6: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm6); size += 32; break;
			case YMM7: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm7); size += 32; break;
			case YMM8: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm8); size += 32; break;
			case YMM9: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm9); size += 32; break;
			case YMM10: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm10); size += 32; break;
			case YMM11: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm11); size += 32; break;
			case YMM12: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm12); size += 32; break;
			case YMM13: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm13); size += 32; break;
			case YMM14: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm14); size += 32; break;
			case YMM15: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm15); size += 32; break;
#ifdef POLYHOOK2_ARCH_AVX512
			case YMM16: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm16); size += 32; break;
			case YMM17: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm17); size += 32; break;
			case YMM18: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm18); size += 32; break;
			case YMM19: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm19); size += 32; break;
			case YMM20: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm20); size += 32; break;
			case YMM21: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm21); size += 32; break;
			case YMM22: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm22); size += 32; break;
			case YMM23: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm23); size += 32; break;
			case YMM24: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm24); size += 32; break;
			case YMM25: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm25); size += 32; break;
			case YMM26: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm26); size += 32; break;
			case YMM27: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm27); size += 32; break;
			case YMM28: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm28); size += 32; break;
			case YMM29: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm29); size += 32; break;
			case YMM30: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm30); size += 32; break;
			case YMM31: a.sub(xsp, 32); a.vmovaps(ymmword_ptr(xsp), ymm31); size += 32; break;
#endif // POLYHOOK2_ARCH_AVX512
#endif // POLYHOOK2_ARCH_AVX

			// ========================================================================
			// >> 512-bit ZMM registers
			// ========================================================================
#ifdef POLYHOOK2_ARCH_AVX512
			case ZMM0: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm0); size += 64; break;
			case ZMM1: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm1); size += 64; break;
			case ZMM2: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm2); size += 64; break;
			case ZMM3: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm3); size += 64; break;
			case ZMM4: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm4); size += 64; break;
			case ZMM5: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm5); size += 64; break;
			case ZMM6: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm6); size += 64; break;
			case ZMM7: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm7); size += 64; break;
			case ZMM8: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm8); size += 64; break;
			case ZMM9: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm9); size += 64; break;
			case ZMM10: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm10); size += 64; break;
			case ZMM11: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm11); size += 64; break;
			case ZMM12: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm12); size += 64; break;
			case ZMM13: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm13); size += 64; break;
			case ZMM14: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm14); size += 64; break;
			case ZMM15: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm15); size += 64; break;
			case ZMM16: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm16); size += 64; break;
			case ZMM17: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm17); size += 64; break;
			case ZMM18: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm18); size += 64; break;
			case ZMM19: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm19); size += 64; break;
			case ZMM20: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm20); size += 64; break;
			case ZMM21: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm21); size += 64; break;
			case ZMM22: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm22); size += 64; break;
			case ZMM23: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm23); size += 64; break;
			case ZMM24: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm24); size += 64; break;
			case ZMM25: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm25); size += 64; break;
			case ZMM26: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm26); size += 64; break;
			case ZMM27: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm27); size += 64; break;
			case ZMM28: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm28); size += 64; break;
			case ZMM29: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm29); size += 64; break;
			case ZMM30: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm30); size += 64; break;
			case ZMM31: a.sub(xsp, 64); a.vmovaps(zmmword_ptr(xsp), zmm31); size += 64; break;
#endif // POLYHOOK2_ARCH_AVX512

			// ========================================================================
			// >> 16-bit Segment registers
			// ========================================================================
			case CS: a.push(cs); size += p; break;
			case SS: a.push(ss); size += p; break;
			case DS: a.push(ds); size += p; break;
			case ES: a.push(es); size += p; break;
			case FS: a.push(fs); size += p; break;
			case GS: a.push(gs); size += p; break;

			case EF: a.pushf(); size += p; break;

			default: POLYHOOK2_FATAL("Unsupported register.\n");
		}
	}
	return size;
}

void Callback::restoreScratchRegisters(Assembler& a) {
	for (auto it = s_registers.rbegin(); it != s_registers.rend(); ++it) {
		switch (*it) {
			// ========================================================================
			// >> 8-bit General purpose registers
			// ========================================================================
			case AL: a.pop(al); break;
			case CL: a.pop(cl); break;
			case DL: a.pop(dl); break;
			case BL: a.pop(bl); break;
#ifdef POLYHOOK2_ARCH_X64
			case SPL: a.pop(spl); break;
			case BPL: a.pop(bpl); break;
			case SIL: a.pop(sil); break;
			case DIL: a.pop(dil); break;
			case R8B: a.pop(r8b); break;
			case R9B: a.pop(r9b); break;
			case R10B: a.pop(r10b); break;
			case R11B: a.pop(r11b); break;
			case R12B: a.pop(r12b); break;
			case R13B: a.pop(r13b); break;
			case R14B: a.pop(r14b); break;
			case R15B: a.pop(r15b); break;
#endif
			case AH: a.pop(ah); break;
			case CH: a.pop(ch); break;
			case DH: a.pop(dh); break;
			case BH: a.pop(bh); break;

			// ========================================================================
			// >> 16-bit General purpose registers
			// ========================================================================
			case AX: a.pop(ax); break;
			case CX: a.pop(cx); break;
			case DX: a.pop(dx); break;
			case BX: a.pop(bx); break;
			case SP: a.pop(sp); break;
			case BP: a.pop(bp); break;
			case SI: a.pop(si); break;
			case DI: a.pop(di); break;
#ifdef POLYHOOK2_ARCH_X64
			case R8W: a.pop(r8w); break;
			case R9W: a.pop(r9w); break;
			case R10W: a.pop(r10w); break;
			case R11W: a.pop(r11w); break;
			case R12W: a.pop(r12w); break;
			case R13W: a.pop(r13w); break;
			case R14W: a.pop(r14w); break;
			case R15W: a.pop(r15w); break;
#endif

			// ========================================================================
			// >> 32-bit General purpose registers
			// ========================================================================
			case EAX: a.pop(eax); break;
			case ECX: a.pop(ecx); break;
			case EDX: a.pop(edx); break;
			case EBX: a.pop(ebx); break;
			case ESP: a.pop(esp); break;
			case EBP: a.pop(ebp); break;
			case ESI: a.pop(esi); break;
			case EDI: a.pop(edi); break;
#ifdef POLYHOOK2_ARCH_X64
			case R8D: a.pop(r8d); break;
			case R9D: a.pop(r9d); break;
			case R10D: a.pop(r10d); break;
			case R11D: a.pop(r11d); break;
			case R12D: a.pop(r12d); break;
			case R13D: a.pop(r13d); break;
			case R14D: a.pop(r14d); break;
			case R15D: a.pop(r15d); break;
#endif

			// ========================================================================
			// >> 64-bit General purpose registers
			// ========================================================================
#ifdef POLYHOOK2_ARCH_X64
			case RAX: a.pop(rax); break;
			case RCX: a.pop(rcx); break;
			case RDX: a.pop(rdx); break;
			case RBX: a.pop(rbx); break;
			case RSP: a.pop(xsp); break;
			case RBP: a.pop(rbp); break;
			case RSI: a.pop(rsi); break;
			case RDI: a.pop(rdi); break;

			case R8: a.pop(r8); break;
			case R9: a.pop(r9); break;
			case R10: a.pop(r10); break;
			case R11: a.pop(r11); break;
			case R12: a.pop(r12); break;
			case R13: a.pop(r13); break;
			case R14: a.pop(r14); break;
			case R15: a.pop(r15); break;
#endif

			// ========================================================================
			// >> 64-bit MM (MMX) registers
			// ========================================================================
			case MM0: a.movq(mm0, qword_ptr(xsp)); a.add(xsp, 8); break;
			case MM1: a.movq(mm1, qword_ptr(xsp)); a.add(xsp, 8); break;
			case MM2: a.movq(mm2, qword_ptr(xsp)); a.add(xsp, 8); break;
			case MM3: a.movq(mm3, qword_ptr(xsp)); a.add(xsp, 8); break;
			case MM4: a.movq(mm4, qword_ptr(xsp)); a.add(xsp, 8); break;
			case MM5: a.movq(mm5, qword_ptr(xsp)); a.add(xsp, 8); break;
			case MM6: a.movq(mm6, qword_ptr(xsp)); a.add(xsp, 8); break;
			case MM7: a.movq(mm7, qword_ptr(xsp)); a.add(xsp, 8); break;

			// ========================================================================
			// >> 128-bit XMM registers
			// ========================================================================
			case XMM0: a.movaps(xmm0, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM1: a.movaps(xmm1, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM2: a.movaps(xmm2, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM3: a.movaps(xmm3, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM4: a.movaps(xmm4, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM5: a.movaps(xmm5, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM6: a.movaps(xmm6, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM7: a.movaps(xmm7, xmmword_ptr(xsp)); a.add(xsp, 16); break;
#ifdef POLYHOOK2_ARCH_X64
			case XMM8: a.movaps(xmm8, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM9: a.movaps(xmm9, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM10: a.movaps(xmm10, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM11: a.movaps(xmm11, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM12: a.movaps(xmm12, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM13: a.movaps(xmm13, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM14: a.movaps(xmm14, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM15: a.movaps(xmm15, xmmword_ptr(xsp)); a.add(xsp, 16); break;
#endif
#ifdef POLYHOOK2_ARCH_AVX512
			case XMM16: a.movaps(xmm16, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM17: a.movaps(xmm17, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM18: a.movaps(xmm18, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM19: a.movaps(xmm19, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM20: a.movaps(xmm20, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM21: a.movaps(xmm21, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM22: a.movaps(xmm22, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM23: a.movaps(xmm23, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM24: a.movaps(xmm24, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM25: a.movaps(xmm25, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM26: a.movaps(xmm26, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM27: a.movaps(xmm27, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM28: a.movaps(xmm28, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM29: a.movaps(xmm29, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM30: a.movaps(xmm30, xmmword_ptr(xsp)); a.add(xsp, 16); break;
			case XMM31: a.movaps(xmm31, xmmword_ptr(xsp)); a.add(xsp, 16); break;
#endif // POLYHOOK2_ARCH_AVX512

			// ========================================================================
			// >> 256-bit YMM registers
			// ========================================================================
#ifdef POLYHOOK2_ARCH_AVX
			case YMM0: a.vmovaps(ymm0, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM1: a.vmovaps(ymm1, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM2: a.vmovaps(ymm2, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM3: a.vmovaps(ymm3, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM4: a.vmovaps(ymm4, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM5: a.vmovaps(ymm5, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM6: a.vmovaps(ymm6, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM7: a.vmovaps(ymm7, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM8: a.vmovaps(ymm8, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM9: a.vmovaps(ymm9, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM10: a.vmovaps(ymm10, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM11: a.vmovaps(ymm11, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM12: a.vmovaps(ymm12, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM13: a.vmovaps(ymm13, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM14: a.vmovaps(ymm14, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM15: a.vmovaps(ymm15, ymmword_ptr(xsp)); a.add(xsp, 32); break;
#ifdef POLYHOOK2_ARCH_AVX512
			case YMM16: a.vmovaps(ymm16, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM17: a.vmovaps(ymm17, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM18: a.vmovaps(ymm18, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM19: a.vmovaps(ymm19, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM20: a.vmovaps(ymm20, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM21: a.vmovaps(ymm21, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM22: a.vmovaps(ymm22, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM23: a.vmovaps(ymm23, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM24: a.vmovaps(ymm24, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM25: a.vmovaps(ymm25, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM26: a.vmovaps(ymm26, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM27: a.vmovaps(ymm27, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM28: a.vmovaps(ymm28, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM29: a.vmovaps(ymm29, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM30: a.vmovaps(ymm30, ymmword_ptr(xsp)); a.add(xsp, 32); break;
			case YMM31: a.vmovaps(ymm31, ymmword_ptr(xsp)); a.add(xsp, 32); break;
#endif // POLYHOOK2_ARCH_AVX512
#endif // POLYHOOK2_ARCH_AVX

			// ========================================================================
			// >> 512-bit ZMM registers
			// ========================================================================
#ifdef POLYHOOK2_ARCH_AVX512
			case ZMM0: a.vmovaps(zmm0, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM1: a.vmovaps(zmm1, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM2: a.vmovaps(zmm2, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM3: a.vmovaps(zmm3, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM4: a.vmovaps(zmm4, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM5: a.vmovaps(zmm5, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM6: a.vmovaps(zmm6, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM7: a.vmovaps(zmm7, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM8: a.vmovaps(zmm8, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM9: a.vmovaps(zmm9, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM10: a.vmovaps(zmm10, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM11: a.vmovaps(zmm11, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM12: a.vmovaps(zmm12, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM13: a.vmovaps(zmm13, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM14: a.vmovaps(zmm14, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM15: a.vmovaps(zmm15, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM16: a.vmovaps(zmm16, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM17: a.vmovaps(zmm17, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM18: a.vmovaps(zmm18, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM19: a.vmovaps(zmm19, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM20: a.vmovaps(zmm20, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM21: a.vmovaps(zmm21, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM22: a.vmovaps(zmm22, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM23: a.vmovaps(zmm23, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM24: a.vmovaps(zmm24, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM25: a.vmovaps(zmm25, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM26: a.vmovaps(zmm26, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM27: a.vmovaps(zmm27, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM28: a.vmovaps(zmm28, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM29: a.vmovaps(zmm29, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM30: a.vmovaps(zmm30, zmmword_ptr(xsp)); a.add(xsp, 64); break;
			case ZMM31: a.vmovaps(zmm31, zmmword_ptr(xsp)); a.add(xsp, 64); break;
#endif // POLYHOOK2_ARCH_AVX512

			// ========================================================================
			// >> 16-bit Segment registers
			// ========================================================================
			case CS: a.pop(cs); break;
			case SS: a.pop(ss); break;
			case DS: a.pop(ds); break;
			case ES: a.pop(es); break;
			case FS: a.pop(fs); break;
			case GS: a.pop(gs); break;

			case EF: a.popf(); break;

			default: POLYHOOK2_FATAL("Unsupported register.\n");
		}
	}
}
#endif