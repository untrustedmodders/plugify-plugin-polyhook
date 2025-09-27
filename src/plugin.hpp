#pragma once

#include "callback.hpp"
#include "hash.hpp"

#include <plg/plugin.hpp>
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
#include <queue>
#include <chrono>

namespace PLH {
	class PolyHookPlugin final : public plg::IPluginEntry, public MemAccessor {
	public:
		void OnPluginStart() final;
		void OnPluginUpdate(std::chrono::milliseconds dt) final;
		void OnPluginEnd() final;

		Callback* hookDetour(void* pFunc, DataType returnType, std::span<const DataType> arguments, uint8_t vaIndex);
		Callback* hookVirtual(void* pClass, int index, DataType returnType, std::span<const DataType> arguments, uint8_t vaIndex);
		Callback* hookVirtual(void* pClass, void* pFunc, DataType returnType, std::span<const DataType> arguments, uint8_t vaIndex);

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
		struct VHook {
			std::unique_ptr<IHook> vtable;
			std::unordered_map<int, std::unique_ptr<Callback>> callbacks;
			VFuncMap redirectMap;
			VFuncMap origVFuncs;
		};
		std::unordered_map<void*, VHook> m_vhooks;
		struct DHook {
			std::unique_ptr<NatDetour> detour;
			std::unique_ptr<Callback> callback;
		};
		std::unordered_map<void*, DHook> m_detours;
		using Clock = std::chrono::steady_clock;
		using TimePoint = std::chrono::time_point<Clock>;
		struct DelayedRemoval {
			std::unique_ptr<Callback> callback;
			TimePoint when;

			bool operator<(const DelayedRemoval& t) const { return when > t.when; }
		};
		std::priority_queue<DelayedRemoval> m_removals;
		std::mutex m_mutex;
	};
}
