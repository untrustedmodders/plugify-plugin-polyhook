#pragma once

#include "callback.hpp"

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
#include <print>

namespace PLH {
	class PolyHookPlugin final : public plg::Plugin, public MemAccessor {
	public:
		plg::PluginResult OnPluginStart() override;
		plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
		plg::PluginResult OnPluginEnd() override;

		Callback* hookDetour(void* pFunc, const Signature& sig);
		Callback* hookDetour(void* pFunc, std::string_view name = {});
		template<typename T>
		Callback* hookVirtual(void* pClass, int index, const Signature& sig);
		template<typename T>
		Callback* hookVirtual(void* pClass, void* pFunc, const Signature& sig);

		bool unhookDetour(void* pFunc);
		template<typename T>
		bool unhookVirtual(void* pClass, int index);
		template<typename T>
		bool unhookVirtual(void* pClass, void* pFunc);

		Callback* findDetour(void* pFunc) const;
		Callback* findVirtual(void* pClass, void* pFunc) const;
		Callback* findVirtual(void* pClass, int index) const;

		void unhookAll();
		void unhookAllVirtual(void* pClass);

		int getVirtualIndex(void* pFunc, ProtFlag flag = ProtFlag::RWX) const;
		std::string_view getError() const noexcept;

	private:
		struct VHook {
			std::unique_ptr<IHook> vtable;
			std::unordered_map<int, std::unique_ptr<Callback>> callbacks;
			VFuncMap redirectMap;
			VFuncMap origVFuncs;
			void* klass{};
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
		mutable std::shared_mutex m_mutex;
		std::string m_error;
	};
}
