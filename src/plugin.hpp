#pragma once

#include "callback.hpp"
#include "hash.hpp"

#include <plugify/cpp_plugin.hpp>
#include <plugin_export.h>

#include <polyhook2/Detour/NatDetour.hpp>
#include <polyhook2/Tests/TestEffectTracker.hpp>
#include <polyhook2/Virtuals/VTableSwapHook.hpp>
#include <polyhook2/Virtuals/VFuncSwapHook.hpp>
#include <polyhook2/PolyHookOsIncludes.hpp>

#include <asmjit/asmjit.h>
#include <unordered_map>
#include <memory>
#include <mutex>

namespace PLH {
#if PLH_SOURCEHOOK
	struct ShPointer {
		void* ptr;
	};
	struct ShDeleter {
		void operator()(ShPointer* dp) const;
	};
#endif

	class PolyHookPlugin final : public plg::IPluginEntry, public MemAccessor {
	public:
		void OnPluginStart() final;
		void OnPluginEnd() final;

		Callback* hookDetour(void* pFunc, DataType returnType, std::span<const DataType> arguments);
		Callback* hookVirtual(void* pClass, int index, DataType returnType, std::span<const DataType> arguments);
		Callback* hookVirtual(void* pClass, void* pFunc, DataType returnType, std::span<const DataType> arguments);

		bool unhookDetour(void* pFunc);
		bool unhookVirtual(void* pClass, int index);
		bool unhookVirtual(void* pClass, void* pFunc);

		Callback* findDetour(void* pFunc) const;
		Callback* findVirtual(void* pClass, void* pFunc) const;
		Callback* findVirtual(void* pClass, int index) const;

		void unhookAll();
		void unhookAllVirtual(void* pClass);

		int getVirtualTableIndex(void* pFunc, ProtFlag flag = RWX) const;

	private:
		std::shared_ptr<asmjit::JitRuntime> m_jitRuntime;
#if PLH_SOURCEHOOK
		struct SHook {
			std::unique_ptr<ShPointer, ShDeleter> pre;
			std::unique_ptr<ShPointer, ShDeleter> post;
			std::unique_ptr<Callback> callback;
		};
		std::unordered_map<std::pair<void*, int>, SHook> m_shooks;
#endif
		struct VHook {
			std::unique_ptr<VTableSwapHook> vtable;
			std::map<int, Callback> callbacks;
			VFuncMap redirectMap;
			VFuncMap origVFuncs;
		};
		std::unordered_map<void*, VHook> m_vhooks;
		struct DHook {
			std::unique_ptr<NatDetour> detour;
			std::unique_ptr<Callback> callback;
		};
		std::unordered_map<void*, DHook> m_detours;
		std::mutex m_mutex;
	};
}
