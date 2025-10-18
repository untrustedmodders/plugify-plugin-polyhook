#include "plugin.hpp"

PLH::PolyHookPlugin g_polyHookPlugin;
EXPOSE_PLUGIN(PLUGIN_API, PLH::PolyHookPlugin, &g_polyHookPlugin)

using namespace PLH;
using enum CallbackType;
using namespace std::chrono_literals;

static void PreCallback(Callback* callback, const Callback::Parameters* params, size_t count, const Callback::Return* ret, ReturnFlag* flag) {
	ReturnAction returnAction = ReturnAction::Ignored;

	auto callbacks = callback->getCallbacks(Pre);
	for (const auto& func : callbacks) {
		ReturnAction result = func(callback, params, static_cast<int32_t>(count), ret, Pre);
		if (result > returnAction)
			returnAction = result;
	}

	if (!callback->areCallbacksRegistered(Post)) {
		*flag |= ReturnFlag::NoPost;
	}
	if (returnAction >= ReturnAction::Supercede) {
		*flag |= ReturnFlag::Supercede;
	}
}

static void PostCallback(Callback* callback, const Callback::Parameters* params, size_t count, const Callback::Return* ret, ReturnFlag*) {
	auto callbacks = callback->getCallbacks(Post);

	for (const auto& func : callbacks) {
		func(callback, params, static_cast<int32_t>(count), ret, Post);
	}
}

void PolyHookPlugin::OnPluginStart() {
}

void PolyHookPlugin::OnPluginUpdate([[maybe_unused]] std::chrono::milliseconds dt) {
	if (!m_removals.empty() && Clock::now() >= m_removals.top().when) {
		m_removals.pop();
	}
}

void PolyHookPlugin::OnPluginEnd() {
	unhookAll();

	while (!m_removals.empty()) {
		m_removals.pop();
	}
}

Callback* PolyHookPlugin::hookDetour(void* pFunc, DataType returnType, std::span<const DataType> arguments, uint8_t varIndex) {
	if (!pFunc)
		return nullptr;

	std::unique_lock lock(m_mutex);

	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		return it->second.callback.get();
	}

	auto callback = std::make_unique<Callback>(returnType, arguments);

	uint64_t JIT = callback->getJitFunc(returnType, arguments, &PreCallback, &PostCallback, varIndex);

	auto error = callback->getError();
	if (!error.empty()) {
		std::fputs(error.data(), stderr);
		std::terminate();
	}

	auto detour = std::make_unique<NatDetour>((uint64_t) pFunc, JIT, callback->getTrampolineHolder());
	if (!detour->hook())
		return nullptr;

	return m_detours.emplace(pFunc, DHook{std::move(detour), std::move(callback)}).first->second.callback.get();
}

template<typename T>
Callback* PolyHookPlugin::hookVirtual(void* pClass, int index, DataType returnType, std::span<const DataType> arguments, uint8_t varIndex) {
	if (!pClass || index == -1)
		return nullptr;

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

	auto& [vtable, callbacks, redirectMap, origVFuncs] = it->second;

	auto& callback = callbacks.emplace(index, std::make_unique<Callback>(returnType, arguments)).first->second;
	uint64_t JIT = callback->getJitFunc(returnType, arguments, &PreCallback, &PostCallback, varIndex);

	auto error = callback->getError();
	if (!error.empty()) {
		std::fputs(error.data(), stderr);
		std::terminate();
	}

	redirectMap[index] = JIT;

	vtable = std::make_unique<T>((uint64_t) pClass, redirectMap, &origVFuncs);
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
Callback* PolyHookPlugin::hookVirtual(void* pClass, void* pFunc, DataType returnType, std::span<const DataType> arguments, uint8_t varIndex) {
	return hookVirtual<T>(pClass, getVirtualIndex(pFunc), returnType, arguments, varIndex);
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
		auto& [vtable, callbacks, redirectMap, origVFuncs] = it->second;

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

		vtable = std::make_unique<T>((uint64_t) pClass, redirectMap, &origVFuncs);
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

#if defined(__clang)
PLUGIFY_WARN_IGNORE("-Wreturn-type-c-linkage")
#elif defined(_MSC_VER)
PLUGIFY_WARN_IGNORE(4190)
#endif

extern "C" {
	// Detour
	PLUGIN_API Callback* HookDetour(void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookDetour(pFunc, returnType, arguments, static_cast<uint8_t>(varIndex));
	}
	PLUGIN_API bool UnhookDetour(void* pFunc) {
		return g_polyHookPlugin.unhookDetour(pFunc);
	}

	// Virtual (VTableSwapHook)
	PLUGIN_API Callback* HookVirtualTable(void* pClass, int index, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookVirtual<VTableSwapHook>(pClass, index, returnType, arguments, static_cast<uint8_t>(varIndex));
	}
	PLUGIN_API Callback* HookVirtualTable2(void* pClass, void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookVirtual<VTableSwapHook>(pClass, pFunc, returnType, arguments, static_cast<uint8_t>(varIndex));
	}
	PLUGIN_API bool UnhookVirtualTable(void* pClass, int index) {
		return g_polyHookPlugin.unhookVirtual<VTableSwapHook>(pClass, index);
	}
	PLUGIN_API bool UnhookVirtualTable2(void* pClass, void* pFunc) {
		return g_polyHookPlugin.unhookVirtual<VTableSwapHook>(pClass, pFunc);
	}

	// Virtual (VFuncSwapHook)
	PLUGIN_API Callback* HookVirtualFunc(void* pClass, int index, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookVirtual<VFuncSwapHook>(pClass, index, returnType, arguments, static_cast<uint8_t>(varIndex));
	}
	PLUGIN_API Callback* HookVirtualFunc2(void* pClass, void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookVirtual<VFuncSwapHook>(pClass, pFunc, returnType, arguments, static_cast<uint8_t>(varIndex));
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

	PLUGIN_API bool GetArgumentBool(const Callback::Parameters* params, size_t index) { return params->getArg<bool>(index); }
	PLUGIN_API int8_t GetArgumentInt8(const Callback::Parameters* params, size_t index) { return params->getArg<int8_t>(index); }
	PLUGIN_API uint8_t GetArgumentUInt8(const Callback::Parameters* params, size_t index) { return params->getArg<uint8_t>(index); }
	PLUGIN_API int16_t GetArgumentInt16(const Callback::Parameters* params, size_t index) { return params->getArg<int16_t>(index); }
	PLUGIN_API uint16_t GetArgumentUInt16(const Callback::Parameters* params, size_t index) { return params->getArg<uint16_t>(index); }
	PLUGIN_API int32_t GetArgumentInt32(const Callback::Parameters* params, size_t index) { return params->getArg<int32_t>(index); }
	PLUGIN_API uint32_t GetArgumentUInt32(const Callback::Parameters* params, size_t index) { return params->getArg<uint32_t>(index); }
	PLUGIN_API int64_t GetArgumentInt64(const Callback::Parameters* params, size_t index) { return params->getArg<int64_t>(index); }
	PLUGIN_API uint64_t GetArgumentUInt64(const Callback::Parameters* params, size_t index) { return params->getArg<uint64_t>(index); }
	PLUGIN_API float GetArgumentFloat(const Callback::Parameters* params, size_t index) { return params->getArg<float>(index); }
	PLUGIN_API double GetArgumentDouble(const Callback::Parameters* params, size_t index) { return params->getArg<double>(index); }
	PLUGIN_API void* GetArgumentPointer(const Callback::Parameters* params, size_t index) { return params->getArg<void*>(index); }
	PLUGIN_API plg::string GetArgumentString(const Callback::Parameters* params, size_t index) {
		const char* str = params->getArg<const char*>(index);
		if (str == nullptr)
			return {};
		else
			return str;
	}
	PLUGIN_API plg::any GetArgument(Callback* callback, const Callback::Parameters* params, size_t index) {
		switch (callback->getReturnType()) {
			case DataType::Void:
				return {};
			case DataType::Bool:
				return params->getArg<bool>(index);
			case DataType::Int8:
				return params->getArg<int8_t>(index);
			case DataType::UInt8:
				return params->getArg<uint8_t>(index);
			case DataType::Int16:
				return params->getArg<int16_t>(index);
			case DataType::UInt16:
				return params->getArg<uint16_t>(index);
			case DataType::Int32:
				return params->getArg<int32_t>(index);
			case DataType::UInt32:
				return params->getArg<uint32_t>(index);
			case DataType::Int64:
				return params->getArg<int64_t>(index);
			case DataType::UInt64:
				return params->getArg<uint64_t>(index);
			case DataType::Float:
				return params->getArg<float>(index);
			case DataType::Double:
				return params->getArg<double>(index);
			case DataType::Pointer:
				return params->getArg<void*>(index);
			case DataType::String: {
				const char* str = params->getArg<const char*>(index);
				if (str == nullptr)
					return {};
				else
					return str;
			}
			default:
				return {};
		}
	}

	PLUGIN_API void SetArgumentBool(const Callback::Parameters* params, size_t index, bool value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt8(const Callback::Parameters* params, size_t index, int8_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt8(const Callback::Parameters* params, size_t index, uint8_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt16(const Callback::Parameters* params, size_t index, int16_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt16(const Callback::Parameters* params, size_t index, uint16_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt32(const Callback::Parameters* params, size_t index, int32_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt32(const Callback::Parameters* params, size_t index, uint32_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt64(const Callback::Parameters* params, size_t index, int64_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt64(const Callback::Parameters* params, size_t index, uint64_t value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentFloat(const Callback::Parameters* params, size_t index, float value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentDouble(const Callback::Parameters* params, size_t index, double value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentPointer(const Callback::Parameters* params, size_t index, void* value) { params->setArg(index, value); }
	PLUGIN_API void SetArgumentString(Callback* callback, const Callback::Parameters* params, size_t index, const plg::string& value) {
		params->setArg(index, plg::get<plg::string>(callback->setStorage(index, value)).c_str());
	}

	PLUGIN_API void SetArgument(Callback* callback, const Callback::Parameters* params, size_t index, const plg::any& value) {
		plg::visit([&](const auto& v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (is_none_type_v<T>) {
				params->setArg(index, nullptr);
			} else if constexpr (std::is_arithmetic_v<T> || std::is_pointer_v<T>) {
				params->setArg(index, v);
			} else if constexpr (is_math_type_v<T>) {
				params->setArg(index, &plg::get<T>(callback->setStorage(index, value)).data);
			} else if constexpr (is_vector_type_v<T>) {
				params->setArg(index, plg::get<T>(callback->setStorage(index, value)).data());
			} else if constexpr (std::is_same_v<T, plg::string>) {
				params->setArg(index, plg::get<T>(callback->setStorage(index, value)).c_str());
			} else {
				std::fputs("Type not supported", stderr);
				std::terminate();
			}
		}, value);
	}

	PLUGIN_API bool GetReturnBool(const Callback::Return* ret) { return ret->getRet<bool>(); }
	PLUGIN_API int8_t GetReturnInt8(const Callback::Return* ret) { return ret->getRet<int8_t>(); }
	PLUGIN_API uint8_t GetReturnUInt8(const Callback::Return* ret) { return ret->getRet<uint8_t>(); }
	PLUGIN_API int16_t GetReturnInt16(const Callback::Return* ret) { return ret->getRet<int16_t>(); }
	PLUGIN_API uint16_t GetReturnUInt16(const Callback::Return* ret) { return ret->getRet<uint16_t>(); }
	PLUGIN_API int32_t GetReturnInt32(const Callback::Return* ret) { return ret->getRet<int32_t>(); }
	PLUGIN_API uint32_t GetReturnUInt32(const Callback::Return* ret) { return ret->getRet<uint32_t>(); }
	PLUGIN_API int64_t GetReturnInt64(const Callback::Return* ret) { return ret->getRet<int64_t>(); }
	PLUGIN_API uint64_t GetReturnUInt64(const Callback::Return* ret) { return ret->getRet<uint64_t>(); }
	PLUGIN_API float GetReturnFloat(const Callback::Return* ret) { return ret->getRet<float>(); }
	PLUGIN_API double GetReturnDouble(const Callback::Return* ret) { return ret->getRet<double>(); }
	PLUGIN_API void* GetReturnPointer(const Callback::Return* ret) { return ret->getRet<void*>(); }
	PLUGIN_API plg::string GetReturnString(const Callback::Return* ret) {
		const char* str = ret->getRet<const char*>();
		if (str == nullptr)
			return {};
		else
			return str;
	}

	PLUGIN_API plg::any GetReturn(Callback* callback, const Callback::Return* ret) {
		switch (callback->getReturnType()) {
		case DataType::Void:
			return {};
		case DataType::Bool:
			return ret->getRet<bool>();
		case DataType::Int8:
			return ret->getRet<int8_t>();
		case DataType::UInt8:
			return ret->getRet<uint8_t>();
		case DataType::Int16:
			return ret->getRet<int16_t>();
		case DataType::UInt16:
			return ret->getRet<uint16_t>();
		case DataType::Int32:
			return ret->getRet<int32_t>();
		case DataType::UInt32:
			return ret->getRet<uint32_t>();
		case DataType::Int64:
			return ret->getRet<int64_t>();
		case DataType::UInt64:
			return ret->getRet<uint64_t>();
		case DataType::Float:
			return ret->getRet<float>();
		case DataType::Double:
			return ret->getRet<double>();
		case DataType::Pointer:
			return ret->getRet<void*>();
		case DataType::String: {
			const char* str = ret->getRet<const char*>();
			if (str == nullptr)
				return {};
			else
				return str;
		}
		default:
			return {};
		}
	}

	PLUGIN_API void SetReturnBool(const Callback::Return* ret, bool value) { ret->setRet(value); }
	PLUGIN_API void SetReturnInt8(const Callback::Return* ret, int8_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnUInt8(const Callback::Return* ret, uint8_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnInt16(const Callback::Return* ret, int16_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnUInt16(const Callback::Return* ret, uint16_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnInt32(const Callback::Return* ret, int32_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnUInt32(const Callback::Return* ret, uint32_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnInt64(const Callback::Return* ret, int64_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnUInt64(const Callback::Return* ret, uint64_t value) { ret->setRet(value); }
	PLUGIN_API void SetReturnFloat(const Callback::Return* ret, float value) { ret->setRet(value); }
	PLUGIN_API void SetReturnDouble(const Callback::Return* ret, double value) { ret->setRet(value); }
	PLUGIN_API void SetReturnPointer(const Callback::Return* ret, void* value) { ret->setRet(value); }
	PLUGIN_API void SetReturnString(Callback* callback, const Callback::Return* ret, const plg::string& value) {
		ret->setRet(plg::get<plg::string>(callback->setStorage(-1, value)).c_str());
	}

	PLUGIN_API void SetReturn(Callback* callback, const Callback::Return* ret, const plg::any& value) {
		plg::visit([&](const auto& v) {
			using T = std::decay_t<decltype(v)>;
			if constexpr (is_none_type_v<T>) {
				ret->setRet(nullptr);
			} else if constexpr (std::is_arithmetic_v<T> || std::is_pointer_v<T>) {
				ret->setRet(v);
			} else if constexpr (is_math_type_v<T>) {
				ret->setRet(&plg::get<T>(callback->setStorage(-1, value)));
			} else if constexpr (is_vector_type_v<T>) {
				ret->setRet(plg::get<T>(callback->setStorage(-1, value)).data());
			} else if constexpr (std::is_same_v<T, plg::string>) {
				ret->setRet(plg::get<T>(callback->setStorage(-1, value)).c_str());
			} else {
				std::fputs("Type not supported", stderr);
				std::terminate();
			}
		}, value);
	}
}

PLUGIFY_WARN_POP()
