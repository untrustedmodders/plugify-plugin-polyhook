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
	class PolyHookPlugin final : public plg::IPluginEntry, public MemAccessor {
		void OnPluginStart() final;
		void OnPluginEnd() final;
		
	public:
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

		void* findOriginalAddr(void* pClass, void* pAddr);
		int getVirtualTableIndex(void* pFunc, ProtFlag flag = RWX) const;

	private:
		std::shared_ptr<asmjit::JitRuntime> m_jitRuntime;
		std::unordered_map<std::pair<void*, int>, std::unique_ptr<Callback>> m_callbacks;
		std::unordered_map<void*, std::pair<VFuncMap, VFuncMap>> m_tables;
		std::unordered_map<void*, std::unique_ptr<IHook>> m_vhooks;
		std::unordered_map<void*, std::unique_ptr<NatDetour>> m_detours;
		std::mutex m_mutex;
	};
}
