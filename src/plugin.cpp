#include "plugin.hpp"

PLH::PolyHookPlugin g_polyHookPlugin;
PLUGIFY_PLUGIN(PLUGIN_API, &g_polyHookPlugin)

using namespace PLH;
using enum CallbackType;
using namespace std::chrono_literals;
using namespace std::string_literals;

constexpr size_t XMM = 16 / sizeof(void*);

enum RegisterType : size_t {
	XMM0 = 0 * XMM,
	XMM1 = 1 * XMM,
	XMM2 = 2 * XMM,
	XMM3 = 3 * XMM,
	XMM4 = 4 * XMM,
	XMM5 = 5 * XMM,
	XMM6 = 6 * XMM,
	XMM7 = 7 * XMM,
#ifdef POLYHOOK2_ARCH_X64
	XMM8 = 8 * XMM,
	XMM9 = 9 * XMM,
	XMM10 = 10 * XMM,
	XMM11 = 11 * XMM,
	XMM12 = 12 * XMM,
	XMM13 = 13 * XMM,
	XMM14 = 14 * XMM,
	XMM15 = 15 * XMM,
	R15 = 16 * XMM,
	R14,
	R13,
	R12,
	R11,
	R10,
	R9,
	R8,
	RDI,
	RSI,
	RBP,
	RBX,
	RDX,
	RCX,
	RAX,
	RFLAGS,
#else
	EDI = 8 * XMM,
	ESI,
	EBP,
	EBX,
	EDX,
	ECX,
	EAX,
	EFLAGS,
#endif
	COUNT
};

static void PreCallback(Callback* callback, uint64_t* p, size_t count, void* r, ReturnFlag* flag) {
	plg::Scope scope(callback->getDebugName(Pre));

	ParametersSpan params(p, count);
	ReturnSlot ret(r, SizeOf(callback->getReturnType()));

	ReturnAction returnAction = ReturnAction::Ignored;
	
	if (callback->areCallbacksRegistered(Pre)) {
		for (auto callbacks = callback->getCallbacks(Pre);
		const auto& func : callbacks) {
		ReturnAction result = func(callback, &params, static_cast<int32_t>(count), &ret, Pre);
		if (result > returnAction)
			returnAction = result;
		}
	}

	if (!callback->areCallbacksRegistered(Post)) {
		*flag |= ReturnFlag::NoPost;
	}
	if (returnAction >= ReturnAction::Supercede) {
		*flag |= ReturnFlag::Supercede;
	}
}

static void PostCallback(Callback* callback, uint64_t* p, size_t count, void* r, ReturnFlag*) {
	plg::Scope scope(callback->getDebugName(Post));

	ParametersSpan params(p, count);
	ReturnSlot ret(r, SizeOf(callback->getReturnType()));

	for (auto callbacks = callback->getCallbacks(Post);
		const auto& func : callbacks) {
		func(callback, &params, static_cast<int32_t>(count), &ret, Post);
	}
}

static void MidCallback(Callback* callback, uintptr_t* p) {
	plg::Scope scope(callback->getDebugName());

	ParametersSpan params(p, COUNT);

	for (constexpr std::array types = { Pre, Post }; const auto& type : types) {
		if (callback->areCallbacksRegistered(type)) {
			for (auto callbacks = callback->getCallbacks(type);
			const auto& func : callbacks) {
				func(callback, &params, COUNT, nullptr, type);
			}
		}
	}
}

plg::PluginResult PolyHookPlugin::OnPluginStart() {
	auto logger = std::make_shared<ErrorLog>();
	logger->setLogLevel(ErrorLevel::SEV);
	Log::registerLogger(logger);
	return {};
}

plg::PluginResult PolyHookPlugin::OnPluginUpdate([[maybe_unused]] std::chrono::milliseconds dt) {
	if (!m_removals.empty() && Clock::now() >= m_removals.top().when) {
		m_removals.pop();
	}
	return {};
}

plg::PluginResult PolyHookPlugin::OnPluginEnd() {
	unhookAll();

	while (!m_removals.empty()) {
		m_removals.pop();
	}
	return {};
}

Callback* PolyHookPlugin::hookDetour(void* pFunc, const Signature& sig) {
	if (!pFunc) {
		m_error = "Invalid func pointer";
		Log::log("polyhook::hookDetour: " + m_error, ErrorLevel::SEV);
		return nullptr;
	}

	std::unique_lock lock(m_mutex);

	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		return it->second.callback.get();
	}

	auto callback = std::make_unique<Callback>(sig);

	uint64_t JIT = callback->getJitFunc(sig, &PreCallback, &PostCallback);

	if (auto error = callback->getError(); !error.empty()) {
		m_error = error;
		Log::log("polyhook::hookDetour: " + m_error, ErrorLevel::SEV);
		return nullptr;
	}

	auto detour = std::make_unique<NatDetour>((uint64_t) pFunc, JIT, callback->getTrampolineHolder());
	if (!detour->hook())
		return nullptr;

	return m_detours.emplace(pFunc, DHook{std::move(detour), std::move(callback)}).first->second.callback.get();
}

Callback* PolyHookPlugin::hookDetour(void* pFunc, std::string_view name) {
	if (!pFunc) {
		m_error = "Invalid func pointer";
		Log::log("polyhook::hookDetour: " + m_error, ErrorLevel::SEV);
		return nullptr;
	}

	std::unique_lock lock(m_mutex);

	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		return it->second.callback.get();
	}

	auto callback = std::make_unique<Callback>(Signature{.name = name});

	uint64_t JIT = callback->getJitFunc(&MidCallback);

	if (auto error = callback->getError(); !error.empty()) {
		m_error = error;
		Log::log("polyhook::hookDetour: " + m_error, ErrorLevel::SEV);
		return nullptr;
	}

	auto detour = std::make_unique<NatDetour>((uint64_t) pFunc, JIT, callback->getTrampolineHolder());
	if (!detour->hook())
		return nullptr;

	return m_detours.emplace(pFunc, DHook{std::move(detour), std::move(callback)}).first->second.callback.get();
}

template<typename T>
Callback* PolyHookPlugin::hookVirtual(void* pClass, int index, const Signature& sig) {
	if (!pClass || index == -1) {
		m_error = "Invalid class or vfunc index";
		Log::log("polyhook::hookVirtual: " + m_error, ErrorLevel::SEV);
		return nullptr;
	}

	std::unique_lock lock(m_mutex);

	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		auto it2 = it->second.callbacks.find(index);
		if (it2 != it->second.callbacks.end()) {
			return it2->second.get();
		} else {
			it->second.vtable->unHook();
		}
	} else {
		it = m_vhooks.emplace(pClass, VHook{}).first;
	}

	auto& [vtable, callbacks, redirectMap, origVFuncs, klass] = it->second;

	auto& callback = callbacks.emplace(index, std::make_unique<Callback>(sig)).first->second;
	uint64_t JIT = callback->getJitFunc(sig, &PreCallback, &PostCallback);

	if (auto error = callback->getError(); !error.empty()) {
		m_error = error;
		Log::log("polyhook::hookVirtual: " + m_error, ErrorLevel::SEV);
		return nullptr;
	}

	redirectMap[index] = JIT;

	klass = pClass;
	if constexpr (std::is_same_v<T, VFuncSwapHook>) {
		vtable = std::make_unique<T>((uint64_t) &klass, redirectMap, &origVFuncs);
	} else {
		vtable = std::make_unique<T>((uint64_t) klass, redirectMap, &origVFuncs);
	}

	if (!vtable->hook()) {
		for (auto& [_, cb] : callbacks) {
			m_removals.push({std::move(cb), Clock::now() + 1s});
		}
		m_vhooks.erase(it);
		return nullptr;
	}

	uint64_t origVFunc = origVFuncs[index];
	*callback->getTrampolineHolder() = origVFunc;

	return callback.get();
}

template<typename T>
Callback* PolyHookPlugin::hookVirtual(void* pClass, void* pFunc, const Signature& sig) {
	return hookVirtual<T>(pClass, getVirtualIndex(pFunc), sig);
}

bool PolyHookPlugin::unhookDetour(void* pFunc) {
	if (!pFunc)
		return false;

	std::unique_lock lock(m_mutex);

	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		auto& [detour, callback] = it->second;
		detour->unHook();
		m_removals.push({std::move(callback), Clock::now() + 1s});
		m_detours.erase(it);
		return true;
	}

	return false;
}

template<typename T>
bool PolyHookPlugin::unhookVirtual(void* pClass, int index) {
	if (!pClass || index == -1)
		return false;

	std::unique_lock lock(m_mutex);

	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		auto& [vtable, callbacks, redirectMap, origVFuncs, klass] = it->second;

		vtable->unHook();
		auto it2 = callbacks.find(index);
		if (it2 != callbacks.end()) {
			m_removals.push({std::move(it2->second), Clock::now() + 1s});
			callbacks.erase(it2);
		}

		redirectMap.erase(index);
		if (redirectMap.empty()) {
			m_vhooks.erase(it);
			return true;
		}

		klass = pClass;
		if constexpr (std::is_same_v<T, VFuncSwapHook>) {
			vtable = std::make_unique<T>((uint64_t) &klass, redirectMap, &origVFuncs);
		} else {
			vtable = std::make_unique<T>((uint64_t) klass, redirectMap, &origVFuncs);
		}

		if (!vtable->hook()) {
			for (auto& [_, cb] : callbacks) {
				m_removals.push({std::move(cb), Clock::now() + 1s});
			}
			m_vhooks.erase(it);
			return false;
		}

		// do not unhook, we just replace our value in map
		return true;
	}

	return false;
}

template<typename T>
bool PolyHookPlugin::unhookVirtual(void* pClass, void* pFunc) {
	return unhookVirtual<T>(pClass, getVirtualIndex(pFunc));
}

Callback* PolyHookPlugin::findDetour(void* pFunc) const {
	std::shared_lock lock(m_mutex);
	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		return it->second.callback.get();
	}
	return nullptr;
}

Callback* PolyHookPlugin::findVirtual(void* pClass, int index) const {
	std::shared_lock lock(m_mutex);
	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		auto it2 = it->second.callbacks.find(index);
		if (it2 != it->second.callbacks.end()) {
			return it2->second.get();
		}
	}

	return nullptr;
}

Callback* PolyHookPlugin::findVirtual(void* pClass, void* pFunc) const {
	return findVirtual(pClass, getVirtualIndex(pFunc));
}

void PolyHookPlugin::unhookAll() {
	std::unique_lock lock(m_mutex);

	m_detours.clear();
	m_vhooks.clear();
}

void PolyHookPlugin::unhookAllVirtual(void* pClass) {
	std::unique_lock lock(m_mutex);

	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		m_vhooks.erase(it);
	}
}

int PolyHookPlugin::getVirtualIndex(void* pFunc, ProtFlag flag) const {
	constexpr size_t size = 12;

	MemoryProtector protector((uint64_t)pFunc, size, flag, *(MemAccessor*)this);

#if defined(__GNUC__) || defined(__clang__)
	struct GCC_MemFunPtr {
		union {
			void* adrr;			// always even
			intptr_t vti_plus1; // vindex+1, always odd
		};
		intptr_t delta;
	};

	int vtindex;
	auto mfp_detail = (GCC_MemFunPtr*)&pFunc;
	if (mfp_detail->vti_plus1 & 1) {
		vtindex = (mfp_detail->vti_plus1 - 1) / sizeof(void*);
	} else {
		vtindex = -1;
	}

	return vtindex;
#elif defined(_MSC_VER)

	// https://www.unknowncheats.me/forum/c-and-c-/102577-vtable-index-pure-virtual-function.html

	// Check whether it's a virtual function call on x86

	// They look like this:a
	//		0:  8b 01                   mov    eax,DWORD PTR [ecx]
	//		2:  ff 60 04                jmp    DWORD PTR [eax+0x4]
	// ==OR==
	//		0:  8b 01                   mov    eax,DWORD PTR [ecx]
	//		2:  ff a0 18 03 00 00       jmp    DWORD PTR [eax+0x318]]

	// However, for vararg functions, they look like this:
	//		0:  8b 44 24 04             mov    eax,DWORD PTR [esp+0x4]
	//		4:  8b 00                   mov    eax,DWORD PTR [eax]
	//		6:  ff 60 08                jmp    DWORD PTR [eax+0x8]
	// ==OR==
	//		0:  8b 44 24 04             mov    eax,DWORD PTR [esp+0x4]
	//		4:  8b 00                   mov    eax,DWORD PTR [eax]
	//		6:  ff a0 18 03 00 00       jmp    DWORD PTR [eax+0x318]
	// With varargs, the this pointer is passed as if it was the first argument

	// On x64
	//		0:  48 8b 01                mov    rax,QWORD PTR [rcx]
	//		3:  ff 60 04                jmp    QWORD PTR [rax+0x4]
	// ==OR==
	//		0:  48 8b 01                mov    rax,QWORD PTR [rcx]
	//		3:  ff a0 18 03 00 00       jmp    QWORD PTR [rax+0x318]

	auto finder = [&](uint8_t* addr) {
		std::unique_ptr<MemoryProtector> protector;

		if (*addr == 0xE9) {
			// May or may not be!
			// Check where it'd jump
			addr += 5 /*size of the instruction*/ + *(uint32_t*)(addr + 1);

			protector = std::make_unique<MemoryProtector>((uint64_t)addr, size, flag, *(MemAccessor*)this);
		}

		bool ok = false;
#ifdef POLYHOOK2_ARCH_X64
		if (addr[0] == 0x48 && addr[1] == 0x8B && addr[2] == 0x01) {
			addr += 3;
			ok = true;
		} else
#endif
		if (addr[0] == 0x8B && addr[1] == 0x01) {
			addr += 2;
			ok = true;
		} else if (addr[0] == 0x8B && addr[1] == 0x44 && addr[2] == 0x24 && addr[3] == 0x04 && addr[4] == 0x8B && addr[5] == 0x00) {
			addr += 6;
			ok = true;
		}

		if (!ok)
			return -1;

		constexpr int PtrSize = static_cast<int>(sizeof(void*));

		if (*addr++ == 0xFF) {
			if (*addr == 0x60)
				return *++addr / PtrSize;
			else if (*addr == 0xA0)
				return int(*((uint32_t*)++addr)) / PtrSize;
			else if (*addr == 0x20)
				return 0;
			else
				return -1;
		}

		return -1;
	};

	return finder((uint8_t*)pFunc);
#else
#error "Compiler not support"
#endif
}

std::string_view PolyHookPlugin::getError() const noexcept {
	return m_error;
}

template<class T>
constexpr bool is_vector_type_v =
		std::is_same_v<T, plg::vector<bool>> ||
		std::is_same_v<T, plg::vector<char>> ||
		std::is_same_v<T, plg::vector<char16_t>> ||
		std::is_same_v<T, plg::vector<int8_t>> ||
		std::is_same_v<T, plg::vector<int16_t>> ||
		std::is_same_v<T, plg::vector<int32_t>> ||
		std::is_same_v<T, plg::vector<int64_t>> ||
		std::is_same_v<T, plg::vector<uint8_t>> ||
		std::is_same_v<T, plg::vector<uint16_t>> ||
		std::is_same_v<T, plg::vector<uint32_t>> ||
		std::is_same_v<T, plg::vector<uint64_t>> ||
		std::is_same_v<T, plg::vector<void*>> ||
		std::is_same_v<T, plg::vector<float>> ||
		std::is_same_v<T, plg::vector<double>> ||
		//std::is_same_v<T, plg::vector<plg::string>> ||
		std::is_same_v<T, plg::vector<plg::variant<plg::none>>> ||
		std::is_same_v<T, plg::vector<plg::vec2>> ||
		std::is_same_v<T, plg::vector<plg::vec3>> ||
		std::is_same_v<T, plg::vector<plg::vec4>> ||
		std::is_same_v<T, plg::vector<plg::mat4x4>>;

template<class T>
constexpr bool is_math_type_v =
		std::is_same_v<T, plg::vec2> ||
		std::is_same_v<T, plg::vec3> ||
		std::is_same_v<T, plg::vec4> ||
		std::is_same_v<T, plg::mat4x4>;

template<class T>
constexpr bool is_none_type_v =
		std::is_same_v<T, plg::invalid> ||
		std::is_same_v<T, plg::none> ||
		std::is_same_v<T, plg::variant<plg::none>> ||
		std::is_same_v<T, plg::function> ||
		std::is_same_v<T, plg::any>;

PLUGIFY_WARN_PUSH()

PLUGIFY_LINKAGE()

extern "C" {
	// Detour
	PLUGIN_API Callback* HookDetour(void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex, const plg::string& name) {
		return g_polyHookPlugin.hookDetour(pFunc, Signature{
			.arguments = arguments,
			.returnType = returnType,
			.varIndex = varIndex,
			.name = name,
		});
	}
	PLUGIN_API Callback* HookDetour2(void* pFunc, const plg::string& name) {
		return g_polyHookPlugin.hookDetour(pFunc, name);
	}
	PLUGIN_API bool UnhookDetour(void* pFunc) {
		return g_polyHookPlugin.unhookDetour(pFunc);
	}

	// Virtual (VTableSwapHook)
	PLUGIN_API Callback* HookVirtualTable(void* pClass, int index, DataType returnType, const plg::vector<DataType>& arguments, int varIndex, const plg::string& name) {
		return g_polyHookPlugin.hookVirtual<VTableSwapHook>(pClass, index, Signature{
			.arguments = arguments,
			.returnType = returnType,
			.varIndex = varIndex,
			.name = name,
		});
	}
	PLUGIN_API Callback* HookVirtualTable2(void* pClass, void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex, const plg::string& name) {
		return g_polyHookPlugin.hookVirtual<VTableSwapHook>(pClass, pFunc, Signature{
			.arguments = arguments,
			.returnType = returnType,
			.varIndex = varIndex,
			.name = name,
		});
	}
	PLUGIN_API bool UnhookVirtualTable(void* pClass, int index) {
		return g_polyHookPlugin.unhookVirtual<VTableSwapHook>(pClass, index);
	}
	PLUGIN_API bool UnhookVirtualTable2(void* pClass, void* pFunc) {
		return g_polyHookPlugin.unhookVirtual<VTableSwapHook>(pClass, pFunc);
	}

	// Virtual (VFuncSwapHook)
	PLUGIN_API Callback* HookVirtualFunc(void* pClass, int index, DataType returnType, const plg::vector<DataType>& arguments, int varIndex, const plg::string& name) {
		return g_polyHookPlugin.hookVirtual<VFuncSwapHook>(pClass, index, Signature{
			.arguments = arguments,
			.returnType = returnType,
			.varIndex = varIndex,
			.name = name,
		});
	}
	PLUGIN_API Callback* HookVirtualFunc2(void* pClass, void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex, const plg::string& name) {
		return g_polyHookPlugin.hookVirtual<VFuncSwapHook>(pClass, pFunc, Signature{
			.arguments = arguments,
			.returnType = returnType,
			.varIndex = varIndex,
			.name = name,
		});
	}
	PLUGIN_API bool UnhookVirtualFunc(void* pClass, int index) {
		return g_polyHookPlugin.unhookVirtual<VFuncSwapHook>(pClass, index);
	}
	PLUGIN_API bool UnhookVirtualFunc2(void* pClass, void* pFunc) {
		return g_polyHookPlugin.unhookVirtual<VFuncSwapHook>(pClass, pFunc);
	}

	PLUGIN_API Callback* FindDetour(void* pFunc) {
		return g_polyHookPlugin.findDetour(pFunc);
	}

	PLUGIN_API Callback* FindVirtual(void* pClass, int index) {
		return g_polyHookPlugin.findVirtual(pClass, index);
	}

	PLUGIN_API Callback* FindVirtual2(void* pClass, void* pFunc) {
		return g_polyHookPlugin.findVirtual(pClass, pFunc);
	}

	PLUGIN_API int GetVirtualIndex(void* pFunc) {
		return g_polyHookPlugin.getVirtualIndex(pFunc);
	}

	PLUGIN_API void UnhookAll() {
		g_polyHookPlugin.unhookAll();
	}

	PLUGIN_API void UnhookAllVirtual(void* pClass) {
		g_polyHookPlugin.unhookAllVirtual(pClass);
	}

	PLUGIN_API plg::string GetError() {
		return g_polyHookPlugin.getError();
	}
	
	PLUGIN_API bool AddCallback(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
		if (callback == nullptr) {
			return false;
		}
		return callback->addCallback(type, handler);
	}

	PLUGIN_API bool AddCallback2(Callback* callback, CallbackType type, Callback::CallbackHandler handler, int priority) {
		if (callback == nullptr) {
			return false;
		}
		return callback->addCallback(type, handler, priority);
	}

	PLUGIN_API bool RemoveCallback(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
		if (callback == nullptr) {
			return false;
		}
		return callback->removeCallback(type, handler);
	}

	PLUGIN_API bool IsCallbackRegistered(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
		if (callback == nullptr) {
			return false;
		}
		return callback->isCallbackRegistered(type, handler);
	}

	PLUGIN_API bool AreCallbacksRegistered(Callback* callback) {
		if (callback == nullptr) {
			return false;
		}
		return callback->areCallbacksRegistered();
	}

	PLUGIN_API void* GetFunctionAddr(Callback* callback) {
		if (callback == nullptr) {
			return nullptr;
		}
		return (void*) *callback->getFunctionHolder();
	}

	PLUGIN_API void* GetOriginalAddr(Callback* callback) {
		if (callback == nullptr) {
			return nullptr;
		}
		return (void*) *callback->getTrampolineHolder();
	}

	PLUGIN_API bool GetArgumentBool(Parameters* params, size_t index) { return params->get<bool>(index); }
	PLUGIN_API int8_t GetArgumentInt8(Parameters* params, size_t index) { return params->get<int8_t>(index); }
	PLUGIN_API uint8_t GetArgumentUInt8(Parameters* params, size_t index) { return params->get<uint8_t>(index); }
	PLUGIN_API int16_t GetArgumentInt16(Parameters* params, size_t index) { return params->get<int16_t>(index); }
	PLUGIN_API uint16_t GetArgumentUInt16(Parameters* params, size_t index) { return params->get<uint16_t>(index); }
	PLUGIN_API int32_t GetArgumentInt32(Parameters* params, size_t index) { return params->get<int32_t>(index); }
	PLUGIN_API uint32_t GetArgumentUInt32(Parameters* params, size_t index) { return params->get<uint32_t>(index); }
	PLUGIN_API int64_t GetArgumentInt64(Parameters* params, size_t index) { return params->get<int64_t>(index); }
	PLUGIN_API uint64_t GetArgumentUInt64(Parameters* params, size_t index) { return params->get<uint64_t>(index); }
	PLUGIN_API float GetArgumentFloat(Parameters* params, size_t index) { return params->get<float>(index); }
	PLUGIN_API double GetArgumentDouble(Parameters* params, size_t index) { return params->get<double>(index); }
	PLUGIN_API void* GetArgumentPointer(Parameters* params, size_t index) { return params->get<void*>(index); }
	PLUGIN_API plg::string GetArgumentString(Parameters* params, size_t index) {
		const char* str = params->get<const char*>(index);
		if (str == nullptr)
			return {};
		else
			return str;
	}
	PLUGIN_API plg::any GetArgument(Callback* callback, Parameters* params, size_t index) {
		switch (callback->getReturnType()) {
			case DataType::Void:
				return {};
			case DataType::Bool:
				return params->get<bool>(index);
			case DataType::Int8:
				return params->get<int8_t>(index);
			case DataType::UInt8:
				return params->get<uint8_t>(index);
			case DataType::Int16:
				return params->get<int16_t>(index);
			case DataType::UInt16:
				return params->get<uint16_t>(index);
			case DataType::Int32:
				return params->get<int32_t>(index);
			case DataType::UInt32:
				return params->get<uint32_t>(index);
			case DataType::Int64:
				return params->get<int64_t>(index);
			case DataType::UInt64:
				return params->get<uint64_t>(index);
			case DataType::Float:
				return params->get<float>(index);
			case DataType::Double:
				return params->get<double>(index);
			case DataType::Pointer:
				return params->get<void*>(index);
			case DataType::String: {
				const char* str = params->get<const char*>(index);
				if (str == nullptr)
					return {};
				else
					return str;
			}
			default:
				return {};
		}
	}

	PLUGIN_API void SetArgumentBool(Parameters* params, size_t index, bool value) { params->set(index, value); }
	PLUGIN_API void SetArgumentInt8(Parameters* params, size_t index, int8_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentUInt8(Parameters* params, size_t index, uint8_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentInt16(Parameters* params, size_t index, int16_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentUInt16(Parameters* params, size_t index, uint16_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentInt32(Parameters* params, size_t index, int32_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentUInt32(Parameters* params, size_t index, uint32_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentInt64(Parameters* params, size_t index, int64_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentUInt64(Parameters* params, size_t index, uint64_t value) { params->set(index, value); }
	PLUGIN_API void SetArgumentFloat(Parameters* params, size_t index, float value) { params->set(index, value); }
	PLUGIN_API void SetArgumentDouble(Parameters* params, size_t index, double value) { params->set(index, value); }
	PLUGIN_API void SetArgumentPointer(Parameters* params, size_t index, void* value) { params->set(index, value); }
	PLUGIN_API void SetArgumentString(Callback* callback, Parameters* params, size_t index, const plg::string& value) {
		params->set(index, plg::get<plg::string>(callback->setStorage(index, value)).c_str());
	}

	PLUGIN_API void SetArgument(Callback* callback, Parameters* params, size_t index, const plg::any& value) {
		plg::visit([&](const auto& v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (is_none_type_v<T>) {
				params->set(index, nullptr);
			} else if constexpr (std::is_arithmetic_v<T> || std::is_pointer_v<T>) {
				params->set(index, v);
			} else if constexpr (is_math_type_v<T>) {
				params->set(index, &plg::get<T>(callback->setStorage(index, value)).data);
			} else if constexpr (is_vector_type_v<T>) {
				params->set(index, plg::get<T>(callback->setStorage(index, value)).data());
			} else if constexpr (std::is_same_v<T, plg::string>) {
				params->set(index, plg::get<T>(callback->setStorage(index, value)).c_str());
			} else {
				Log::log(std::format("{}: Type not supported", __func__), ErrorLevel::SEV);
			}
		}, value);
	}

	PLUGIN_API bool GetReturnBool(ReturnSlot* ret) { return ret->get<bool>(); }
	PLUGIN_API int8_t GetReturnInt8(ReturnSlot* ret) { return ret->get<int8_t>(); }
	PLUGIN_API uint8_t GetReturnUInt8(ReturnSlot* ret) { return ret->get<uint8_t>(); }
	PLUGIN_API int16_t GetReturnInt16(ReturnSlot* ret) { return ret->get<int16_t>(); }
	PLUGIN_API uint16_t GetReturnUInt16(ReturnSlot* ret) { return ret->get<uint16_t>(); }
	PLUGIN_API int32_t GetReturnInt32(ReturnSlot* ret) { return ret->get<int32_t>(); }
	PLUGIN_API uint32_t GetReturnUInt32(ReturnSlot* ret) { return ret->get<uint32_t>(); }
	PLUGIN_API int64_t GetReturnInt64(ReturnSlot* ret) { return ret->get<int64_t>(); }
	PLUGIN_API uint64_t GetReturnUInt64(ReturnSlot* ret) { return ret->get<uint64_t>(); }
	PLUGIN_API float GetReturnFloat(ReturnSlot* ret) { return ret->get<float>(); }
	PLUGIN_API double GetReturnDouble(ReturnSlot* ret) { return ret->get<double>(); }
	PLUGIN_API void* GetReturnPointer(ReturnSlot* ret) { return ret->get<void*>(); }
	PLUGIN_API plg::string GetReturnString(ReturnSlot* ret) {
		const char* str = ret->get<const char*>();
		if (str == nullptr)
			return {};
		else
			return str;
	}

	PLUGIN_API plg::any GetReturn(Callback* callback, ReturnSlot* ret) {
		switch (callback->getReturnType()) {
		case DataType::Void:
			return {};
		case DataType::Bool:
			return ret->get<bool>();
		case DataType::Int8:
			return ret->get<int8_t>();
		case DataType::UInt8:
			return ret->get<uint8_t>();
		case DataType::Int16:
			return ret->get<int16_t>();
		case DataType::UInt16:
			return ret->get<uint16_t>();
		case DataType::Int32:
			return ret->get<int32_t>();
		case DataType::UInt32:
			return ret->get<uint32_t>();
		case DataType::Int64:
			return ret->get<int64_t>();
		case DataType::UInt64:
			return ret->get<uint64_t>();
		case DataType::Float:
			return ret->get<float>();
		case DataType::Double:
			return ret->get<double>();
		case DataType::Pointer:
			return ret->get<void*>();
		case DataType::String: {
			const char* str = ret->get<const char*>();
			if (str == nullptr)
				return {};
			else
				return str;
		}
		default:
			return {};
		}
	}

	PLUGIN_API void SetReturnBool(ReturnSlot* ret, bool value) { ret->set(value); }
	PLUGIN_API void SetReturnInt8(ReturnSlot* ret, int8_t value) { ret->set(value); }
	PLUGIN_API void SetReturnUInt8(ReturnSlot* ret, uint8_t value) { ret->set(value); }
	PLUGIN_API void SetReturnInt16(ReturnSlot* ret, int16_t value) { ret->set(value); }
	PLUGIN_API void SetReturnUInt16(ReturnSlot* ret, uint16_t value) { ret->set(value); }
	PLUGIN_API void SetReturnInt32(ReturnSlot* ret, int32_t value) { ret->set(value); }
	PLUGIN_API void SetReturnUInt32(ReturnSlot* ret, uint32_t value) { ret->set(value); }
	PLUGIN_API void SetReturnInt64(ReturnSlot* ret, int64_t value) { ret->set(value); }
	PLUGIN_API void SetReturnUInt64(ReturnSlot* ret, uint64_t value) { ret->set(value); }
	PLUGIN_API void SetReturnFloat(ReturnSlot* ret, float value) { ret->set(value); }
	PLUGIN_API void SetReturnDouble(ReturnSlot* ret, double value) { ret->set(value); }
	PLUGIN_API void SetReturnPointer(ReturnSlot* ret, void* value) { ret->set(value); }
	PLUGIN_API void SetReturnString(Callback* callback, ReturnSlot* ret, const plg::string& value) {
		ret->set(plg::get<plg::string>(callback->setStorage(-1, value)).c_str());
	}

	PLUGIN_API void SetReturn(Callback* callback, ReturnSlot* ret, const plg::any& value) {
		plg::visit([&](const auto& v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (is_none_type_v<T>) {
				ret->set(nullptr);
			} else if constexpr (std::is_arithmetic_v<T> || std::is_pointer_v<T>) {
				ret->set(v);
			} else if constexpr (is_math_type_v<T>) {
				ret->set(&plg::get<T>(callback->setStorage(-1, value)));
			} else if constexpr (is_vector_type_v<T>) {
				ret->set(plg::get<T>(callback->setStorage(-1, value)).data());
			} else if constexpr (std::is_same_v<T, plg::string>) {
				ret->set(plg::get<T>(callback->setStorage(-1, value)).c_str());
			} else {
				Log::log(std::format("{}: Type not supported", __func__), ErrorLevel::SEV);
			}
		}, value);
	}

	PLUGIN_API bool GetRegisterBool(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<bool>(reg); }
	PLUGIN_API int8_t GetRegisterInt8(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<int8_t>(reg); }
	PLUGIN_API uint8_t GetRegisterUInt8(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<uint8_t>(reg); }
	PLUGIN_API int16_t GetRegisterInt16(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<int16_t>(reg); }
	PLUGIN_API uint16_t GetRegisterUInt16(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<uint16_t>(reg); }
	PLUGIN_API int32_t GetRegisterInt32(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<int32_t>(reg); }
	PLUGIN_API uint32_t GetRegisterUInt32(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<uint32_t>(reg); }
	PLUGIN_API int64_t GetRegisterInt64(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<int64_t>(reg); }
	PLUGIN_API uint64_t GetRegisterUInt64(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<uint64_t>(reg); }
	PLUGIN_API float GetRegisterFloat(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<float>(reg); }
	PLUGIN_API double GetRegisterDouble(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<double>(reg); }
	PLUGIN_API void* GetRegisterPointer(ParametersSpan<uintptr_t>* params, RegisterType reg) { return params->get<void*>(reg); }
	PLUGIN_API plg::string GetRegisterString(ParametersSpan<uintptr_t>* params, RegisterType reg) {
		const char* str = params->get<const char*>(reg);
		if (str == nullptr)
			return {};
		else
			return str;
	}
	PLUGIN_API plg::any GetRegister(Callback* callback, ParametersSpan<uintptr_t>* params, RegisterType reg) {
		switch (callback->getReturnType()) {
			case DataType::Void:
				return {};
			case DataType::Bool:
				return params->get<bool>(reg);
			case DataType::Int8:
				return params->get<int8_t>(reg);
			case DataType::UInt8:
				return params->get<uint8_t>(reg);
			case DataType::Int16:
				return params->get<int16_t>(reg);
			case DataType::UInt16:
				return params->get<uint16_t>(reg);
			case DataType::Int32:
				return params->get<int32_t>(reg);
			case DataType::UInt32:
				return params->get<uint32_t>(reg);
			case DataType::Int64:
				return params->get<int64_t>(reg);
			case DataType::UInt64:
				return params->get<uint64_t>(reg);
			case DataType::Float:
				return params->get<float>(reg);
			case DataType::Double:
				return params->get<double>(reg);
			case DataType::Pointer:
				return params->get<void*>(reg);
			case DataType::String: {
				const char* str = params->get<const char*>(reg);
				if (str == nullptr)
					return {};
				else
					return str;
			}
			default:
				return {};
		}
	}

	PLUGIN_API void SetRegisterBool(ParametersSpan<uintptr_t>* params, RegisterType reg, bool value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterInt8(ParametersSpan<uintptr_t>* params, RegisterType reg, int8_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterUInt8(ParametersSpan<uintptr_t>* params, RegisterType reg, uint8_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterInt16(ParametersSpan<uintptr_t>* params, RegisterType reg, int16_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterUInt16(ParametersSpan<uintptr_t>* params, RegisterType reg, uint16_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterInt32(ParametersSpan<uintptr_t>* params, RegisterType reg, int32_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterUInt32(ParametersSpan<uintptr_t>* params, RegisterType reg, uint32_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterInt64(ParametersSpan<uintptr_t>* params, RegisterType reg, int64_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterUInt64(ParametersSpan<uintptr_t>* params, RegisterType reg, uint64_t value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterFloat(ParametersSpan<uintptr_t>* params, RegisterType reg, float value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterDouble(ParametersSpan<uintptr_t>* params, RegisterType reg, double value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterPointer(ParametersSpan<uintptr_t>* params, RegisterType reg, void* value) { params->set(reg, value); }
	PLUGIN_API void SetRegisterString(Callback* callback, ParametersSpan<uintptr_t>* params, RegisterType reg, const plg::string& value) {
		params->set(reg, plg::get<plg::string>(callback->setStorage(reg, value)).c_str());
	}

	PLUGIN_API void SetRegister(Callback* callback, ParametersSpan<uintptr_t>* params, RegisterType reg, const plg::any& value) {
		plg::visit([&](const auto& v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (is_none_type_v<T>) {
				params->set(reg, nullptr);
			} else if constexpr (std::is_arithmetic_v<T> || std::is_pointer_v<T>) {
				params->set(reg, v);
			} else if constexpr (is_math_type_v<T>) {
				params->set(reg, &plg::get<T>(callback->setStorage(reg, value)).data);
			} else if constexpr (is_vector_type_v<T>) {
				params->set(reg, plg::get<T>(callback->setStorage(reg, value)).data());
			} else if constexpr (std::is_same_v<T, plg::string>) {
				params->set(reg, plg::get<T>(callback->setStorage(reg, value)).c_str());
			} else {
				Log::log(std::format("{}: Type not supported", __func__), ErrorLevel::SEV);
			}
		}, value);
	}
}

PLUGIFY_WARN_POP()
