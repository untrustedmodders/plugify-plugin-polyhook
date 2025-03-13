#include "callback.hpp"

#include "polyhook2/MemProtector.hpp"

#include <thread>
#include <immintrin.h>

template<typename T>
constexpr asmjit::TypeId getTypeIdx() noexcept {
	return static_cast<asmjit::TypeId>(asmjit::TypeUtils::TypeIdOfT<T>::kTypeId);
}

asmjit::TypeId PLH::Callback::getTypeId(DataType type) {
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
			return asmjit::TypeId::kUIntPtr;
	}
	return asmjit::TypeId::kVoid;
}

uint64_t PLH::Callback::getJitFunc(const asmjit::FuncSignature& sig, const CallbackEntry pre, const CallbackEntry post) {
	if (m_functionPtr) {
		return m_functionPtr;
	}

	auto rt = m_rt.lock();
	if (!rt) {
		m_errorCode = "JitRuntime invalid";
		return 0;
	}

	/*AsmJit is smart enough to track register allocations and will forward
	  the proper registers the right values and fixup any it dirtied earlier.
	  This can only be done if it knows the signature, and ABI, so we give it 
	  them. It also only does this mapping for calls, so we need to generate 
	  calls on our boundaries of transfers when we want argument order correct
	  (ABI stuff is managed for us when calling C code within this project via host mode).
	  It also does stack operations for us including alignment, shadow space, and
	  arguments, everything really. Manual stack push/pop is not supported using
	  the AsmJit compiler, so we must create those nodes, and insert them into
	  the Node list manually to not corrupt the compiler's tracking of things.

	  Inside the compiler, before endFunc only virtual registers may be used. Any
	  concrete physical registers will not have their liveness tracked, so will
	  be spoiled and must be manually marked dirty. After endFunc ONLY concrete
	  physical registers may be inserted as nodes.
	*/
	asmjit::CodeHolder code;
	code.init(rt->environment(), rt->cpuFeatures());
	
	// initialize function
	asmjit::x86::Compiler cc(&code);            
	asmjit::FuncNode* func = cc.addFunc(sig);              

	/*asmjit::StringLogger log;
	auto kFormatFlags =
			asmjit::FormatFlags::kMachineCode | asmjit::FormatFlags::kExplainImms | asmjit::FormatFlags::kRegCasts
			| asmjit::FormatFlags::kHexImms | asmjit::FormatFlags::kHexOffsets  | asmjit::FormatFlags::kPositions;
	
	log.addFlags(kFormatFlags);
	code.setLogger(&log);*/

#if PLUGIFY_IS_RELEASE
	// too small to really need it
	func->frame().resetPreservedFP();
#endif

	// Create labels
	asmjit::Label supercede = cc.newLabel();
	asmjit::Label noPost = cc.newLabel();

	// map argument slots to registers, following abi.
	std::vector<asmjit::x86::Reg> argRegisters;
	argRegisters.reserve( sig.argCount());
	for (uint32_t argIdx = 0; argIdx < sig.argCount(); argIdx++) {
		const auto argType = sig.args()[argIdx];

		asmjit::x86::Reg arg;
		if (asmjit::TypeUtils::isInt(argType)) {
			arg = cc.newUIntPtr();
		} else if (asmjit::TypeUtils::isFloat(argType)) {
			arg = cc.newXmm();
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		func->setArg(argIdx, arg);
		argRegisters.push_back(std::move(arg));
	}

	// setup the stack structure to hold arguments for user callback
	uint32_t stackSize = (uint32_t)(sizeof(uint64_t) * sig.argCount());
	asmjit::x86::Mem argsStack = cc.newStack(stackSize, 16);
	asmjit::x86::Mem argsStackIdx(argsStack);

	// assigns some register as index reg
	asmjit::x86::Gp i = cc.newUIntPtr();

	// stackIdx <- stack[i].
	argsStackIdx.setIndex(i);

	// r/w are sizeof(uint64_t) width now
	argsStackIdx.setSize(sizeof(uint64_t));

	// set i = 0
	cc.mov(i, 0);
	//// mov from arguments registers into the stack structure
	for (uint32_t argIdx = 0; argIdx < sig.argCount(); argIdx++) {
		const auto argType = sig.args()[argIdx];

		// have to cast back to explicit register types to gen right mov type
		if (asmjit::TypeUtils::isInt(argType)) {
			cc.mov(argsStackIdx, argRegisters.at(argIdx).as<asmjit::x86::Gp>());
		} else if(asmjit::TypeUtils::isFloat(argType)) {
			cc.movq(argsStackIdx, argRegisters.at(argIdx).as<asmjit::x86::Xmm>());
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, sizeof(uint64_t));
	}

	auto callbackSig = asmjit::FuncSignature::build<void, Callback*, Parameters*, Property*, Return*>();

	// get pointer to callback and pass it to the user callback
	asmjit::x86::Gp argCallback = cc.newUIntPtr("argCallback");
	cc.mov(argCallback, this);

	// get pointer to stack structure and pass it to the user callback
	asmjit::x86::Gp argStruct = cc.newUIntPtr("argStruct");
	cc.lea(argStruct, argsStack);

	// create buffer for property struct
	asmjit::x86::Mem propStack = cc.newStack(sizeof(uint64_t), 16);
	asmjit::x86::Gp propStruct = cc.newUIntPtr("propStruct");
	cc.lea(propStruct, propStack);

	// create buffer for return struct
	asmjit::x86::Mem retStack = cc.newStack(sizeof(uint64_t), 16);
	asmjit::x86::Gp retStruct = cc.newUIntPtr("retStruct");
	cc.lea(retStruct, retStack);

	{
		asmjit::x86::Mem propStackIdx(propStack);
		propStackIdx.setSize(sizeof(uint64_t));
		Property property{ (int32_t) sig.argCount(), ReturnFlag::Default };
		cc.mov(propStackIdx, *(int64_t*) &property);
	}

	asmjit::InvokeNode* invokePreNode;

	// Call pre callback
	cc.invoke(&invokePreNode, (uint64_t)pre, callbackSig);

	// call to user provided function (use ABI of host compiler)
	invokePreNode->setArg(0, argCallback);
	invokePreNode->setArg(1, argStruct);
	invokePreNode->setArg(2, propStruct);
	invokePreNode->setArg(3, retStruct);

	asmjit::x86::Gp propFlag = cc.newInt32();

	{
		asmjit::x86::Mem propStackIdx(propStack);
		propStackIdx.setSize(sizeof(ReturnFlag));
		propStackIdx.setOffset(sizeof(int32_t));
		cc.mov(propFlag, propStackIdx);
	}

	cc.test(propFlag, ReturnFlag::Supercede);
	cc.jnz(supercede);

	// mov from arguments stack structure into regs
	cc.mov(i, 0); // reset idx
	for (uint32_t argIdx = 0; argIdx < sig.argCount(); argIdx++) {
		const auto argType = sig.args()[argIdx];

		if (asmjit::TypeUtils::isInt(argType)) {
			cc.mov(argRegisters.at(argIdx).as<asmjit::x86::Gp>(), argsStackIdx);
		} else if (asmjit::TypeUtils::isFloat(argType)) {
			cc.movq(argRegisters.at(argIdx).as<asmjit::x86::Xmm>(), argsStackIdx);
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, sizeof(uint64_t));
	}

	// deref the trampoline ptr (holder must live longer, must be concrete reg since push later)
	asmjit::x86::Gp origPtr = cc.zbx();
	cc.mov(origPtr, (uintptr_t)getTrampolineHolder());
	cc.mov(origPtr, asmjit::x86::ptr(origPtr));

	asmjit::InvokeNode* origInvokeNode;
	cc.invoke(&origInvokeNode, origPtr, sig);
	for (uint32_t argIdx = 0; argIdx < sig.argCount(); argIdx++) {
		origInvokeNode->setArg(argIdx, argRegisters.at(argIdx));
	}

	if (sig.hasRet()) {
		asmjit::x86::Reg retRegister;
		if (asmjit::TypeUtils::isInt(sig.ret())) {
			retRegister = cc.newUIntPtr();
		} else {
			retRegister = cc.newXmm();
		}
		origInvokeNode->setRet(0, retRegister);

		asmjit::x86::Mem retStackIdx(retStack);
		retStackIdx.setSize(sizeof(uint64_t));
		if (asmjit::TypeUtils::isInt(sig.ret())) {
			cc.mov(retStackIdx, retRegister.as<asmjit::x86::Gp>());
		} else {
			cc.movq(retStackIdx, retRegister.as<asmjit::x86::Xmm>());
		}
	}

	// this code will be executed if a callback returns Supercede
	cc.bind(supercede);

	cc.test(propFlag, ReturnFlag::NoPost);
	cc.jnz(noPost);

	asmjit::InvokeNode* invokePostNode;

	cc.invoke(&invokePostNode, (uint64_t)post, callbackSig);

	// call to user provided function (use ABI of host compiler)
	invokePostNode->setArg(0, argCallback);
	invokePostNode->setArg(1, argStruct);
	invokePostNode->setArg(2, propStruct);
	invokePostNode->setArg(3, retStruct);

	// mov from arguments stack structure into regs
	cc.mov(i, 0); // reset idx
	for (uint32_t argIdx = 0; argIdx < sig.argCount(); argIdx++) {
		const auto argType = sig.args()[argIdx];

		if (asmjit::TypeUtils::isInt(argType)) {
			cc.mov(argRegisters.at(argIdx).as<asmjit::x86::Gp>(), argsStackIdx);
		} else if (asmjit::TypeUtils::isFloat(argType)) {
			cc.movq(argRegisters.at(argIdx).as<asmjit::x86::Xmm>(), argsStackIdx);
		} else {
			m_errorCode = "Parameters wider than 64bits not supported";
			return 0;
		}

		// next structure slot (+= sizeof(uint64_t))
		cc.add(i, sizeof(uint64_t));
	}

	cc.bind(noPost);

	if (sig.hasRet()) {
		asmjit::x86::Mem retStackIdx(retStack);
		retStackIdx.setSize(sizeof(uint64_t));
		if (asmjit::TypeUtils::isInt(sig.ret())) {
			asmjit::x86::Gp tmp = cc.newUIntPtr();
			cc.mov(tmp, retStackIdx);
			cc.ret(tmp);
		} else {
			asmjit::x86::Xmm tmp = cc.newXmm();
			cc.movq(tmp, retStackIdx);
			cc.ret(tmp);
		}
	}

	cc.func()->frame().addDirtyRegs(origPtr);
	
	cc.endFunc();

	cc.finalize();

	if (asmjit::Error err = rt->add(&m_functionPtr, &code)) {
		m_functionPtr = 0;
		m_errorCode = asmjit::DebugUtils::errorAsString(err);
		return 0;
	}

	//Log::log("JIT Stub:\n" + std::string(log.data()), ErrorLevel::INFO);
	return m_functionPtr;
}

uint64_t PLH::Callback::getJitFunc(const DataType retType, std::span<const DataType> paramTypes, const CallbackEntry pre, const CallbackEntry post) {
	asmjit::FuncSignature sig(asmjit::CallConvId::kHost, asmjit::FuncSignature::kNoVarArgs, getTypeId(retType));
	for (const DataType& type : paramTypes) {
		sig.addArg(getTypeId(type));
	}
	return getJitFunc(sig, pre, post);
}

template<typename E>
constexpr auto to_integral(E e) -> std::underlying_type_t<E> {
	return static_cast<std::underlying_type_t<E>>(e);
}

bool PLH::Callback::addCallback(const CallbackType type, const CallbackHandler callback) {
	if (!callback)
		return false;

	std::unique_lock lock(m_mutex);

	std::vector<CallbackHandler>& callbacks = m_callbacks[to_integral(type)];

	for (const CallbackHandler c : callbacks) {
		if (c == callback) {
			return false;
		}
	}

	callbacks.push_back(callback);
	return true;
}

bool PLH::Callback::removeCallback(const CallbackType type, const CallbackHandler callback) {
	if (!callback)
		return false;

	std::unique_lock lock(m_mutex);

	std::vector<CallbackHandler>& callbacks = m_callbacks[to_integral(type)];

	for (size_t i = 0; i < callbacks.size(); i++) {
		if (callbacks[i] == callback) {
			callbacks.erase(callbacks.begin() + static_cast<ptrdiff_t>(i));
			return true;
		}
	}

	return false;
}

bool PLH::Callback::isCallbackRegistered(const CallbackType type, const CallbackHandler callback) const {
	if (!callback)
		return false;

	const std::vector<CallbackHandler>& callbacks = m_callbacks[to_integral(type)];

	for (const CallbackHandler c : callbacks) {
		if (c == callback)
			return true;
	}

	return false;
}

bool PLH::Callback::areCallbacksRegistered(const CallbackType type) const {
	return !m_callbacks[to_integral(type)].empty();
}

bool PLH::Callback::areCallbacksRegistered() const {
	return areCallbacksRegistered(CallbackType::Pre) || areCallbacksRegistered(CallbackType::Post);
}

PLH::Callback::Callbacks PLH::Callback::getCallbacks(const CallbackType type) {
	return { m_callbacks[to_integral(type)], std::shared_lock(m_mutex) };
}

uint64_t* PLH::Callback::getTrampolineHolder() {
	return &m_trampolinePtr;
}

uint64_t* PLH::Callback::getFunctionHolder() {
	return &m_functionPtr;
}

std::string_view PLH::Callback::getError() const {
	return !m_functionPtr && m_errorCode ? m_errorCode : "";
}

const std::string& PLH::Callback::store(std::string_view str) {
	std::unique_lock lock(m_mutex);
	return *m_storage.emplace_back(std::make_unique<std::string>(str));
}

void PLH::Callback::cleanup() {
	m_storage.clear();
}

PLH::Callback::Callback(std::weak_ptr<asmjit::JitRuntime> rt) : m_rt(std::move(rt)) {
}

PLH::Callback::~Callback() {
	int spin_count = 0;
	while (m_mutex.has_shared_locks()) {
		if (++spin_count < 16) {
			_mm_pause();
		} else {
			std::this_thread::yield();
			spin_count = 0;
		}
	}
	if (auto rt = m_rt.lock()) {
		if (m_functionPtr) {
			rt->release(m_functionPtr);
		}
	}
}