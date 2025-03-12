#include "plugin.hpp"

static PLH::PolyHookPlugin g_polyHookPlugin;
EXPOSE_PLUGIN(PLUGIN_API, PLH::PolyHookPlugin, &g_polyHookPlugin)

using namespace PLH;

static void PreCallback(Callback* callback, const Callback::Parameters* params, Callback::Property* property, const Callback::Return* ret) {
	ReturnAction returnAction = ReturnAction::Ignored;

	auto [callbacks, lock] = callback->getCallbacks(CallbackType::Pre);

	for (const auto& cb : callbacks) {
		ReturnAction result = cb(CallbackType::Pre, params, property->count, ret);
		if (result > returnAction)
			returnAction = result;
	}

	if (!callback->areCallbacksRegistered(CallbackType::Post)) {
		property->flag |= ReturnFlag::NoPost;
	}
	if (returnAction >= ReturnAction::Supercede) {
		property->flag |= ReturnFlag::Supercede;
	}
}

static void PostCallback(Callback* callback, const Callback::Parameters* params, Callback::Property* property, const Callback::Return* ret) {
	auto [callbacks, lock] = callback->getCallbacks(CallbackType::Post);

	for (const auto& cb : callbacks) {
		cb(CallbackType::Post, params, property->count, ret);
	}
}

void PolyHookPlugin::OnPluginStart() {
	m_jitRuntime = std::make_unique<asmjit::JitRuntime>();
}

void PolyHookPlugin::OnPluginEnd() {
	unhookAll();
}

Callback* PolyHookPlugin::hookDetour(void* pFunc, DataType returnType, std::span<const DataType> arguments) {
	if (!pFunc)
		return nullptr;

	std::lock_guard m_lock(m_mutex);

	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		auto it2 = m_callbacks.find(std::pair{it->second.get(), -1});
		if (it2 != m_callbacks.end()) {
			return it2->second.get();
		}
	}

	auto callback = std::make_unique<Callback>(m_jitRuntime);
	auto error = callback->getError();
	if (!error.empty()) {
		std::puts(error.data());
		std::terminate();
		return nullptr;
	}

	uint64_t JIT = callback->getJitFunc(returnType, arguments, &PreCallback, &PostCallback);

	auto detour = std::make_unique<NatDetour>((uint64_t) pFunc, JIT, callback->getTrampolineHolder());
	if (!detour->hook())
		return nullptr;

	void* key = m_detours.emplace(pFunc, std::move(detour)).first->second.get();
	return m_callbacks.emplace(std::pair{key, -1}, std::move(callback)).first->second.get();
}

Callback* PolyHookPlugin::hookVirtual(void* pClass, int index, DataType returnType, std::span<const DataType> arguments) {
	if (!pClass || index == -1)
		return nullptr;

	std::lock_guard m_lock(m_mutex);

	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		auto it2 = m_callbacks.find(std::pair{it->second.get(), index});
		if (it2 != m_callbacks.end()) {
			return it2->second.get();
		} else {
			m_vhooks.erase(it);
		}
	}

	auto callback = std::make_unique<Callback>(m_jitRuntime);
	auto error = callback->getError();
	if (!error.empty()) {
		std::puts(error.data());
		std::terminate();
		return nullptr;
	}

	uint64_t JIT = callback->getJitFunc(returnType, arguments, &PreCallback, &PostCallback);

	auto& [redirectMap, origVFuncs] = m_tables[pClass];
	redirectMap[index] = JIT;

	auto vtable = std::make_unique<VFuncSwapHook>((uint64_t) pClass, redirectMap, &origVFuncs);
	if (!vtable->hook())
		return nullptr;

	uint64_t origVFunc = origVFuncs[index];
	*callback->getTrampolineHolder() = origVFunc;

	void* key = m_vhooks.emplace(pClass, std::move(vtable)).first->second.get();
	return m_callbacks.emplace(std::pair{key, index}, std::move(callback)).first->second.get();
}

Callback* PolyHookPlugin::hookVirtual(void* pClass, void* pFunc, DataType returnType, std::span<const DataType> arguments) {
	return hookVirtual(pClass, getVirtualTableIndex(pFunc), returnType, arguments);
}

bool PolyHookPlugin::unhookDetour(void* pFunc) {
	if (!pFunc)
		return false;

	std::lock_guard m_lock(m_mutex);

	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		auto& detour = it->second;
		detour->unHook();
		m_callbacks.erase(std::pair{detour.get(), -1});
		m_detours.erase(it);
		return true;
	}

	return false;
}

bool PolyHookPlugin::unhookVirtual(void* pClass, int index) {
	if (!pClass || index == -1)
		return false;

	std::lock_guard m_lock(m_mutex);

	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		auto& vtable = it->second;
		vtable->unHook();
		m_callbacks.erase(std::pair{vtable.get(), index});
		//m_vhooks.erase(it);

		auto it2 = m_tables.find(pClass);
		if (it2 != m_tables.end()) {
			auto& [redirectMap, origVFuncs] = it2->second;
			redirectMap.erase(index);
			if (redirectMap.empty()) {
				m_tables.erase(it2);
				m_vhooks.erase(it);
				return true;
			}

			vtable = std::make_unique<VFuncSwapHook>((uint64_t) pClass, redirectMap, &origVFuncs);
			if (!vtable->hook()) {
				m_vhooks.erase(it);
				return false;
			}

			// do not unhook, we just replace our value in map
		}

		return true;
	}

	return false;
}

bool PolyHookPlugin::unhookVirtual(void* pClass, void* pFunc) {
	return unhookVirtual(pClass, getVirtualTableIndex(pFunc));
}

Callback* PolyHookPlugin::findDetour(void* pFunc) const {
	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		auto it2 = m_callbacks.find(std::pair{it->second.get(), -1});
		if (it2 != m_callbacks.end()) {
			return it2->second.get();
		}
	}
	return nullptr;
}

Callback* PolyHookPlugin::findVirtual(void* pClass, int index) const {
	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		auto it2 = m_callbacks.find(std::pair{it->second.get(), index});
		if (it2 != m_callbacks.end()) {
			return it2->second.get();
		}
	}
	return nullptr;
}

Callback* PolyHookPlugin::findVirtual(void* pClass, void* pFunc) const {
	return findVirtual(pClass, getVirtualTableIndex(pFunc));
}

void PolyHookPlugin::unhookAll() {
	std::lock_guard m_lock(m_mutex);

	m_detours.clear();
	m_vhooks.clear();
	m_tables.clear();
	m_callbacks.clear();
}

void PolyHookPlugin::unhookAllVirtual(void* pClass) {
	std::lock_guard m_lock(m_mutex);

	auto it = m_vhooks.find(pClass);
	if (it != m_vhooks.end()) {
		auto it2 = m_tables.find(pClass);
		if (it2 != m_tables.end()) {
			for (const auto& [index, func] : it2->second.first) {
				m_callbacks.erase(std::pair{it->second.get(), index});
			}
			m_tables.erase(it2);
		}
		m_vhooks.erase(it);
	}
}

void* PolyHookPlugin::findOriginalAddr(void* pClass, void* pAddr) {
	auto it = m_tables.find(pClass);
	if (it != m_tables.end()) {
		auto& [redirectMap, origVFuncs] = it->second;
		for (const auto& [index, addr] : redirectMap) {
			if ((void*) addr == pAddr) {
				return (void*) origVFuncs[index];
			}
		}
		return pAddr;
	}
	return nullptr;
}

int PolyHookPlugin::getVirtualTableIndex(void* pFunc, ProtFlag flag) const {
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

extern "C"
PLUGIN_API Callback* HookDetour(void* pFunc, DataType returnType, const plg::vector<DataType>& arguments) {
	return g_polyHookPlugin.hookDetour(pFunc, returnType, arguments.const_span());
}

extern "C"
PLUGIN_API Callback* HookVirtual(void* pClass, int index, DataType returnType, const plg::vector<DataType>& arguments) {
	return g_polyHookPlugin.hookVirtual(pClass, index, returnType, arguments.const_span());
}

extern "C"
PLUGIN_API Callback* HookVirtualByFunc(void* pClass, void* pFunc, DataType returnType, const plg::vector<DataType>&  arguments) {
	return g_polyHookPlugin.hookVirtual(pClass, pFunc, returnType, arguments.const_span());
}

extern "C"
PLUGIN_API bool UnhookDetour(void* pFunc) {
	return g_polyHookPlugin.unhookDetour(pFunc);
}

extern "C"
PLUGIN_API bool UnhookVirtual(void* pClass, int index) {
	return g_polyHookPlugin.unhookVirtual(pClass, index);
}

extern "C"
PLUGIN_API bool UnhookVirtualByFunc(void* pClass, void* pFunc) {
	return g_polyHookPlugin.unhookVirtual(pClass, pFunc);
}

extern "C"
PLUGIN_API Callback* FindDetour(void* pFunc) {
	return g_polyHookPlugin.findDetour(pFunc);
}

extern "C"
PLUGIN_API Callback* FindVirtual(void* pClass, int index) {
	return g_polyHookPlugin.findVirtual(pClass, index);
}

extern "C"
PLUGIN_API Callback* FindVirtualByFunc(void* pClass, void* pFunc) {
	return g_polyHookPlugin.findVirtual(pClass, pFunc);
}

extern "C"
PLUGIN_API void* FindOriginalAddr(void* pClass, void* pAddr) {
	return g_polyHookPlugin.findOriginalAddr(pClass, pAddr);
}

extern "C"
PLUGIN_API int GetVTableIndex(void* pFunc) {
	return g_polyHookPlugin.getVirtualTableIndex(pFunc);
}

extern "C"
PLUGIN_API void UnhookAll() {
	return g_polyHookPlugin.unhookAll();
}

extern "C"
PLUGIN_API void UnhookAllVirtual(void* pClass) {
	g_polyHookPlugin.unhookAllVirtual(pClass);
}

extern "C"
PLUGIN_API bool AddCallback(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
	return callback->addCallback(type, handler);
}

extern "C"
PLUGIN_API bool RemoveCallback(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
	return callback->removeCallback(type, handler);
}

extern "C"
PLUGIN_API bool IsCallbackRegistered(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
	return callback->isCallbackRegistered(type, handler);
}

extern "C"
PLUGIN_API bool AreCallbacksRegistered(Callback* callback) {
	return callback->areCallbacksRegistered();
}

extern "C"
PLUGIN_API void* GetCallbackAddr(Callback* callback) {
	return (void*) *callback->getFunctionHolder();
}

extern "C"
PLUGIN_API bool GetArgumentBool(const Callback::Parameters* params, size_t index) { return params->getArg<bool>(index); }
extern "C"
PLUGIN_API int8_t GetArgumentInt8(const Callback::Parameters* params, size_t index) { return params->getArg<int8_t>(index); }
extern "C"
PLUGIN_API uint8_t GetArgumentUInt8(const Callback::Parameters* params, size_t index) { return params->getArg<uint8_t>(index); }
extern "C"
PLUGIN_API int16_t GetArgumentInt16(const Callback::Parameters* params, size_t index) { return params->getArg<int16_t>(index); }
extern "C"
PLUGIN_API uint16_t GetArgumentUInt16(const Callback::Parameters* params, size_t index) { return params->getArg<uint16_t>(index); }
extern "C"
PLUGIN_API int32_t GetArgumentInt32(const Callback::Parameters* params, size_t index) { return params->getArg<int32_t>(index); }
extern "C"
PLUGIN_API uint32_t GetArgumentUInt32(const Callback::Parameters* params, size_t index) { return params->getArg<uint32_t>(index); }
extern "C"
PLUGIN_API int64_t GetArgumentInt64(const Callback::Parameters* params, size_t index) { return params->getArg<int64_t>(index); }
extern "C"
PLUGIN_API uint64_t GetArgumentUInt64(const Callback::Parameters* params, size_t index) { return params->getArg<uint64_t>(index); }
extern "C"
PLUGIN_API float GetArgumentFloat(const Callback::Parameters* params, size_t index) { return params->getArg<float>(index); }
extern "C"
PLUGIN_API double GetArgumentDouble(const Callback::Parameters* params, size_t index) { return params->getArg<double>(index); }
extern "C"
PLUGIN_API void* GetArgumentPointer(const Callback::Parameters* params, size_t index) { return params->getArg<void*>(index); }
extern "C"
PLUGIN_API const char* GetArgumentString(const Callback::Parameters* params, size_t index) { return params->getArg<const char*>(index); }
extern "C"
PLUGIN_API const wchar_t* GetArgumentWString(const Callback::Parameters* params, size_t index) { return params->getArg<const wchar_t*>(index); }

extern "C"
PLUGIN_API void SetArgumentBool(const Callback::Parameters* params, size_t index, bool value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentInt8(const Callback::Parameters* params, size_t index, int8_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentUInt8(const Callback::Parameters* params, size_t index, uint8_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentInt16(const Callback::Parameters* params, size_t index, int16_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentUInt16(const Callback::Parameters* params, size_t index, uint16_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentInt32(const Callback::Parameters* params, size_t index, int32_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentUInt32(const Callback::Parameters* params, size_t index, uint32_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentInt64(const Callback::Parameters* params, size_t index, int64_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentUInt64(const Callback::Parameters* params, size_t index, uint64_t value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentFloat(const Callback::Parameters* params, size_t index, float value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentDouble(const Callback::Parameters* params, size_t index, double value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentPointer(const Callback::Parameters* params, size_t index, void* value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentString(const Callback::Parameters* params, size_t index, const char* value) { return params->setArg(index, value); }
extern "C"
PLUGIN_API void SetArgumentWString(const Callback::Parameters* params, size_t index, const wchar_t* value) { return params->setArg(index, value); }

extern "C"
PLUGIN_API bool GetReturnBool(const Callback::Return* ret) { return ret->getRet<bool>(); }
extern "C"
PLUGIN_API int8_t GetReturnInt8(const Callback::Return* ret) { return ret->getRet<int8_t>(); }
extern "C"
PLUGIN_API uint8_t GetReturnUInt8(const Callback::Return* ret) { return ret->getRet<uint8_t>(); }
extern "C"
PLUGIN_API int16_t GetReturnInt16(const Callback::Return* ret) { return ret->getRet<int16_t>(); }
extern "C"
PLUGIN_API uint16_t GetReturnUInt16(const Callback::Return* ret) { return ret->getRet<uint16_t>(); }
extern "C"
PLUGIN_API int32_t GetReturnInt32(const Callback::Return* ret) { return ret->getRet<int32_t>(); }
extern "C"
PLUGIN_API uint32_t GetReturnUInt32(const Callback::Return* ret) { return ret->getRet<uint32_t>(); }
extern "C"
PLUGIN_API int64_t GetReturnInt64(const Callback::Return* ret) { return ret->getRet<int64_t>(); }
extern "C"
PLUGIN_API uint64_t GetReturnUInt64(const Callback::Return* ret) { return ret->getRet<uint64_t>(); }
extern "C"
PLUGIN_API float GetReturnFloat(const Callback::Return* ret) { return ret->getRet<float>(); }
extern "C"
PLUGIN_API double GetReturnDouble(const Callback::Return* ret) { return ret->getRet<double>(); }
extern "C"
PLUGIN_API void* GetReturnPointer(const Callback::Return* ret) { return ret->getRet<void*>(); }
extern "C"
PLUGIN_API const char* GetReturnString(const Callback::Return* ret) { return ret->getRet<const char*>(); }
extern "C"
PLUGIN_API const wchar_t* GetReturnWString(const Callback::Return* ret) { return ret->getRet<const wchar_t*>(); }

extern "C"
PLUGIN_API void SetReturnBool(const Callback::Return* ret, bool value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnInt8(const Callback::Return* ret, int8_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnUInt8(const Callback::Return* ret, uint8_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnInt16(const Callback::Return* ret, int16_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnUInt16(const Callback::Return* ret, uint16_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnInt32(const Callback::Return* ret, int32_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnUInt32(const Callback::Return* ret, uint32_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnInt64(const Callback::Return* ret, int64_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnUInt64(const Callback::Return* ret, uint64_t value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnFloat(const Callback::Return* ret, float value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnDouble(const Callback::Return* ret, double value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnPointer(const Callback::Return* ret, void* value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnString(const Callback::Return* ret, const char* value) { return ret->setRet(value); }
extern "C"
PLUGIN_API void SetReturnWString(const Callback::Return* ret, const wchar_t* value) { return ret->setRet(value); }