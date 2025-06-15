#include "plugin.hpp"
#include <dynlibutils/module.hpp>

PLH::PolyHookPlugin g_polyHookPlugin;
EXPOSE_PLUGIN(PLUGIN_API, PLH::PolyHookPlugin, &g_polyHookPlugin)

using namespace PLH;
using enum CallbackType;
using namespace std::chrono_literals;

static void PreCallback(Callback* callback, const Callback::Parameters* params, size_t count, const Callback::Return* ret, ReturnFlag* flag) {
	callback->cleanup();

	auto [callbacks, lock] = callback->getCallbacks(Pre);

	ReturnAction returnAction = ReturnAction::Ignored;

	for (const auto& cb : callbacks) {
		ReturnAction result = cb(callback, params, static_cast<int32_t>(count), ret, Pre);
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
	auto [callbacks, lock] = callback->getCallbacks(Post);

	for (const auto& cb : callbacks) {
		cb(callback, params, static_cast<int32_t>(count), ret, Post);
	}
}

#if PLH_SOURCEHOOK
using CreateHookFn = ShPointer* (*)(void* iface, DataType returnType, std::span<const DataType> paramsType, void* function, int offset, bool post);
using DeleteHookFn = void (*)(ShPointer*);
using HookSetResFn = void (*)(ReturnAction);
CreateHookFn CreateHook;
DeleteHookFn DeleteHook;
HookSetResFn HookSetRes;
bool SourceHook = false;

void ShDeleter::operator()(ShPointer* dp) const {
	DeleteHook(dp);
}

static void PreCallback2(Callback* callback, const Callback::Parameters* params, size_t count, const Callback::Return* ret, ReturnFlag*) {
	callback->cleanup();

	auto [callbacks, lock] = callback->getCallbacks(Pre);

	ReturnAction returnAction = ReturnAction::Ignored;

	for (const auto& cb : callbacks) {
		ReturnAction result = cb(callback, params, static_cast<int32_t>(count), ret, Pre);
		if (result > returnAction)
			returnAction = result;
	}

	HookSetRes(returnAction);
}

static void PostCallback2(Callback* callback, const Callback::Parameters* params,  size_t count, const Callback::Return* ret, ReturnFlag*) {
	auto [callbacks, lock] = callback->getCallbacks(Post);

	ReturnAction returnAction = ReturnAction::Ignored;

	for (const auto& cb : callbacks) {
		ReturnAction result = cb(callback, params, static_cast<int32_t>(count), ret, Post);
		if (result > returnAction)
			returnAction = result;
	}

	HookSetRes(returnAction);
}
#endif

void PolyHookPlugin::OnPluginStart() {
	m_jitRuntime = std::make_unique<asmjit::JitRuntime>();

#if PLH_SOURCEHOOK
	DynLibUtils::CModule plugify("plugify");

	auto Plugify_CreateHook = plugify.GetFunctionByName("Plugify_CreateHook");
	if (Plugify_CreateHook) {
		CreateHook = Plugify_CreateHook.CCast<CreateHookFn>();
	}

	auto Plugify_DeleteHook = plugify.GetFunctionByName("Plugify_DeleteHook");
	if (Plugify_DeleteHook) {
		DeleteHook = Plugify_DeleteHook.CCast<DeleteHookFn>();
	}

	auto Plugify_HookSetRes = plugify.GetFunctionByName("Plugify_HookSetRes");
	if (Plugify_HookSetRes) {
		HookSetRes = Plugify_HookSetRes.CCast<HookSetResFn>();
	}

	SourceHook = CreateHook && DeleteHook && HookSetRes;
#endif
}

void PolyHookPlugin::OnPluginUpdate(float dt) {
	if (!m_removals.empty() && Clock::now() >= m_removals.top().when) {
		m_removals.pop();
	}
}

void PolyHookPlugin::OnPluginEnd() {
	unhookAll();

	while (!m_removals.empty()) {
		m_removals.pop();
	}

	m_jitRuntime.reset();

#if PLH_SOURCEHOOK
	SourceHook = false;
#endif
}

Callback* PolyHookPlugin::hookDetour(void* pFunc, DataType returnType, std::span<const DataType> arguments, uint8_t varIndex) {
	if (!pFunc)
		return nullptr;

	std::lock_guard lock(m_mutex);

	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		return it->second.callback.get();
	}

	auto callback = std::make_unique<Callback>(m_jitRuntime);

	uint64_t JIT = callback->getJitFunc(returnType, arguments, &PreCallback, &PostCallback, varIndex);

	auto error = callback->getError();
	if (!error.empty()) {
		std::puts(error.data());
		std::terminate();
	}

	auto detour = std::make_unique<NatDetour>((uint64_t) pFunc, JIT, callback->getTrampolineHolder());
	if (!detour->hook())
		return nullptr;

	return m_detours.emplace(pFunc, DHook{std::move(detour), std::move(callback)}).first->second.callback.get();
}

Callback* PolyHookPlugin::hookVirtual(void* pClass, int index, DataType returnType, std::span<const DataType> arguments, uint8_t varIndex) {
	if (!pClass || index == -1)
		return nullptr;

	std::lock_guard lock(m_mutex);

#if PLH_SOURCEHOOK
	if (SourceHook) {
		auto it = m_shooks.find({pClass, index});
		if (it != m_shooks.end()) {
			return it->second.callback.get();
		}

		auto callback = std::make_unique<Callback>(m_jitRuntime);

		auto [preJIT, postJIT] = callback->getJitFunc2(returnType, arguments, &PreCallback2, &PostCallback2, varIndex);

		auto error = callback->getError();
		if (!error.empty()) {
			std::puts(error.data());
			std::terminate();
		}

		auto pre = std::unique_ptr<ShPointer, ShDeleter>(CreateHook(pClass, returnType, arguments, (void*) preJIT, index, false));
		auto post = std::unique_ptr<ShPointer, ShDeleter>(CreateHook(pClass, returnType, arguments, (void*) postJIT, index, true));

		return m_shooks.emplace(std::pair{pClass, index}, SHook{std::move(pre), std::move(post), std::move(callback)}).first->second.callback.get();
	}
	else
#endif
	{
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

		auto& callback = callbacks.emplace(index, std::make_unique<Callback>(m_jitRuntime)).first->second;
		uint64_t JIT = callback->getJitFunc(returnType, arguments, &PreCallback, &PostCallback, varIndex);

		auto error = callback->getError();
		if (!error.empty()) {
			std::puts(error.data());
			std::terminate();
		}

		redirectMap[index] = JIT;

		vtable = std::make_unique<VTableSwapHook>((uint64_t) pClass, redirectMap, &origVFuncs);
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
}

Callback* PolyHookPlugin::hookVirtual(void* pClass, void* pFunc, DataType returnType, std::span<const DataType> arguments, uint8_t varIndex) {
	return hookVirtual(pClass, getVirtualTableIndex(pFunc), returnType, arguments, varIndex);
}

bool PolyHookPlugin::unhookDetour(void* pFunc) {
	if (!pFunc)
		return false;

	std::lock_guard lock(m_mutex);

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

bool PolyHookPlugin::unhookVirtual(void* pClass, int index) {
	if (!pClass || index == -1)
		return false;

	std::lock_guard lock(m_mutex);

#if PLH_SOURCEHOOK
	if (SourceHook) {
		auto it = m_shooks.find({pClass, index});
		if (it != m_shooks.end()) {
			auto& [pre, post, callback] = it->second;
			m_removals.push({std::move(callback), Clock::now() + 1s});
			m_shooks.erase(it);
			return true;
		}
	}
	else
#endif
	{
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

			vtable = std::make_unique<VTableSwapHook>((uint64_t) pClass, redirectMap, &origVFuncs);
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
	}

	return false;
}

bool PolyHookPlugin::unhookVirtual(void* pClass, void* pFunc) {
	return unhookVirtual(pClass, getVirtualTableIndex(pFunc));
}

Callback* PolyHookPlugin::findDetour(void* pFunc) const {
	auto it = m_detours.find(pFunc);
	if (it != m_detours.end()) {
		return it->second.callback.get();
	}
	return nullptr;
}

Callback* PolyHookPlugin::findVirtual(void* pClass, int index) const {
#if PLH_SOURCEHOOK
	if (SourceHook) {
		auto it = m_shooks.find({pClass, index});
		if (it != m_shooks.end()) {
			return it->second.callback.get();
		}
	}
	else
#endif
	{
		auto it = m_vhooks.find(pClass);
		if (it != m_vhooks.end()) {
			auto it2 = it->second.callbacks.find(index);
			if (it2 != it->second.callbacks.end()) {
				return it2->second.get();
			}
		}
	}

	return nullptr;
}

Callback* PolyHookPlugin::findVirtual(void* pClass, void* pFunc) const {
	return findVirtual(pClass, getVirtualTableIndex(pFunc));
}

void PolyHookPlugin::unhookAll() {
	std::lock_guard lock(m_mutex);

	m_detours.clear();
#if PLH_SOURCEHOOK
	if (SourceHook) {
		m_shooks.clear();
	}
	else
#endif
	{
		m_vhooks.clear();
	}
}

void PolyHookPlugin::unhookAllVirtual(void* pClass) {
	std::lock_guard lock(m_mutex);

#if PLH_SOURCEHOOK
	if (SourceHook) {
		for (auto it = m_shooks.begin(); it != m_shooks.end();) {
			auto& [ptr, index] = it->first;
			if (ptr == pClass) {
				it = m_shooks.erase(it);
			} else {
				++it;
			}
		}
	}
	else
#endif
	{
		auto it = m_vhooks.find(pClass);
		if (it != m_vhooks.end()) {
			m_vhooks.erase(it);
		}
	}
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

PLUGIFY_WARN_PUSH()

#if defined(__clang)
PLUGIFY_WARN_IGNORE("-Wreturn-type-c-linkage")
#elif defined(_MSC_VER)
PLUGIFY_WARN_IGNORE(4190)
#endif

extern "C" {
	PLUGIN_API Callback* HookDetour(void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookDetour(pFunc, returnType, arguments.span(), static_cast<uint8_t>(varIndex));
	}

	PLUGIN_API Callback* HookVirtual(void* pClass, int index, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookVirtual(pClass, index, returnType, arguments.span(), static_cast<uint8_t>(varIndex));
	}

	PLUGIN_API Callback* HookVirtualByFunc(void* pClass, void* pFunc, DataType returnType, const plg::vector<DataType>& arguments, int varIndex) {
		return g_polyHookPlugin.hookVirtual(pClass, pFunc, returnType, arguments.span(), static_cast<uint8_t>(varIndex));
	}

	PLUGIN_API bool UnhookDetour(void* pFunc) {
		return g_polyHookPlugin.unhookDetour(pFunc);
	}

	PLUGIN_API bool UnhookVirtual(void* pClass, int index) {
		return g_polyHookPlugin.unhookVirtual(pClass, index);
	}

	PLUGIN_API bool UnhookVirtualByFunc(void* pClass, void* pFunc) {
		return g_polyHookPlugin.unhookVirtual(pClass, pFunc);
	}

	PLUGIN_API Callback* FindDetour(void* pFunc) {
		return g_polyHookPlugin.findDetour(pFunc);
	}

	PLUGIN_API Callback* FindVirtual(void* pClass, int index) {
		return g_polyHookPlugin.findVirtual(pClass, index);
	}

	PLUGIN_API Callback* FindVirtualByFunc(void* pClass, void* pFunc) {
		return g_polyHookPlugin.findVirtual(pClass, pFunc);
	}

	PLUGIN_API int GetVTableIndex(void* pFunc) {
		return g_polyHookPlugin.getVirtualTableIndex(pFunc);
	}

	PLUGIN_API void UnhookAll() {
		return g_polyHookPlugin.unhookAll();
	}

	PLUGIN_API void UnhookAllVirtual(void* pClass) {
		g_polyHookPlugin.unhookAllVirtual(pClass);
	}

	PLUGIN_API bool AddCallback(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
		return callback->addCallback(type, handler);
	}

	PLUGIN_API bool RemoveCallback(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
		return callback->removeCallback(type, handler);
	}

	PLUGIN_API bool IsCallbackRegistered(Callback* callback, CallbackType type, Callback::CallbackHandler handler) {
		return callback->isCallbackRegistered(type, handler);
	}

	PLUGIN_API bool AreCallbacksRegistered(Callback* callback) {
		return callback->areCallbacksRegistered();
	}

	PLUGIN_API void* GetFunctionAddr(Callback* callback) {
		return (void*) *callback->getFunctionHolder();
	}

	PLUGIN_API void* GetOriginalAddr(Callback* callback) {
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

	PLUGIN_API void SetArgumentBool(const Callback::Parameters* params, size_t index, bool value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt8(const Callback::Parameters* params, size_t index, int8_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt8(const Callback::Parameters* params, size_t index, uint8_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt16(const Callback::Parameters* params, size_t index, int16_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt16(const Callback::Parameters* params, size_t index, uint16_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt32(const Callback::Parameters* params, size_t index, int32_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt32(const Callback::Parameters* params, size_t index, uint32_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentInt64(const Callback::Parameters* params, size_t index, int64_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentUInt64(const Callback::Parameters* params, size_t index, uint64_t value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentFloat(const Callback::Parameters* params, size_t index, float value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentDouble(const Callback::Parameters* params, size_t index, double value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentPointer(const Callback::Parameters* params, size_t index, void* value) { return params->setArg(index, value); }
	PLUGIN_API void SetArgumentString(Callback* callback, const Callback::Parameters* params, size_t index, const plg::string& value) {
		return params->setArg(index, callback->store(value).c_str());
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

	PLUGIN_API void SetReturnBool(const Callback::Return* ret, bool value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnInt8(const Callback::Return* ret, int8_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnUInt8(const Callback::Return* ret, uint8_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnInt16(const Callback::Return* ret, int16_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnUInt16(const Callback::Return* ret, uint16_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnInt32(const Callback::Return* ret, int32_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnUInt32(const Callback::Return* ret, uint32_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnInt64(const Callback::Return* ret, int64_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnUInt64(const Callback::Return* ret, uint64_t value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnFloat(const Callback::Return* ret, float value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnDouble(const Callback::Return* ret, double value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnPointer(const Callback::Return* ret, void* value) { return ret->setRet(value); }
	PLUGIN_API void SetReturnString(Callback* callback, const Callback::Return* ret, const plg::string& value) {
		return ret->setRet(callback->store(value).c_str());
	}
}

PLUGIFY_WARN_POP()
