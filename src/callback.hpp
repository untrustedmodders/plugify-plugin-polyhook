#pragma once

#pragma warning(push, 0)
#include <asmjit/asmjit.h>
#pragma warning( pop )

#pragma warning( disable : 4200)
#include "polyhook2/Enums.hpp"
#include "polyhook2/ErrorLog.hpp"
#include "polyhook2/MemAccessor.hpp"
#include "polyhook2/PolyHookOs.hpp"

#include <array>
#include <vector>
#include <string>
#include <span>
#include <shared_mutex>
#include <atomic>
#include <map>
#include <deque>
#include <thread>

namespace PLH {
	enum class DataType : uint8_t {
		Void,
		Bool,
		Int8,
		UInt8,
		Int16,
		UInt16,
		Int32,
		UInt32,
		Int64,
		UInt64,
		Float,
		Double,
		Pointer,
		String
		// TODO: Add support of POD types
	};

	enum class ReturnAction : int32_t {
		Ignored,  ///< Handler didn't take any action
		Handled,  ///< We did something, but real function should still be called
		Override, ///< Call real function, but use my return value
		Supercede ///< Skip real function; use my return value
	};

	enum class CallbackType : uint8_t {
		Pre,  ///< Callback will be executed before the original function
		Post  ///< Callback will be executed after the original function
	};

	enum class ReturnFlag : int32_t {
		Default = 0, ///< Value means this gives no information about return flag.
		NoPost = 1,
		Supercede = 2,
	};

	class Callback {
	public:
		struct Parameters {
			template<typename T>
			void setArg(const size_t idx, const T val) const {
				*(T*) getArgPtr(idx) = val;
			}

			template<typename T>
			T getArg(const size_t idx) const {
				return *(T*) getArgPtr(idx);
			}

			// asm depends on this specific type
			// we the ILCallback allocates stack space that is set to point here
			volatile uint64_t m_arguments;

		private:
			// must be char* for aliasing rules to work when reading back out
			int8_t* getArgPtr(const size_t idx) const {
				return (int8_t*) &m_arguments + sizeof(uint64_t) * idx;
			}
		};

		struct Return {
			template<typename T>
			void setRet(const T val) const {
				*(T*) getRetPtr() = val;
			}

			template<typename T>
			T getRet() const {
				return *(T*) getRetPtr();
			}
			uint8_t* getRetPtr() const {
				return (uint8_t*) &m_retVal;
			}
			volatile uint64_t m_retVal;
		};

		struct Property {
			int32_t count;
			ReturnFlag flag;
		};

		typedef void (*CallbackEntry)(Callback* callback, const Parameters* params, Property* property, const Return* ret);
		typedef ReturnAction (*CallbackHandler)(Callback* callback, const Parameters* params, int32_t count, const Return* ret, CallbackType type);
		using Callbacks = std::pair<std::vector<CallbackHandler>&, std::shared_lock<std::shared_mutex>>;

		explicit Callback(std::weak_ptr<asmjit::JitRuntime> rt);
		~Callback();

		uint64_t getJitFunc(const asmjit::FuncSignature& sig, CallbackEntry pre, CallbackEntry post);
		uint64_t getJitFunc(DataType retType, std::span<const DataType> paramTypes, CallbackEntry pre, CallbackEntry post, uint8_t vaIndex);

#if PLH_SOURCEHOOK
		std::pair<uint64_t, uint64_t> getJitFunc2(const asmjit::FuncSignature& sig, CallbackEntry pre, CallbackEntry post);
		std::pair<uint64_t, uint64_t> getJitFunc2(DataType retType, std::span<const DataType> paramTypes, CallbackEntry pre, CallbackEntry post, uint8_t vaIndex);

		uint64_t getJitFunc2(const asmjit::FuncSignature& sig, CallbackEntry cb, CallbackType type);
#endif

		uint64_t* getTrampolineHolder() noexcept;
		uint64_t* getFunctionHolder() noexcept;
		Callbacks getCallbacks(CallbackType type) noexcept;
		std::string_view getError() const noexcept;

		const std::string& store(std::string_view str);
		void cleanup();

		bool addCallback(CallbackType type, CallbackHandler callback);
		bool removeCallback(CallbackType type, CallbackHandler callback);
		bool isCallbackRegistered(CallbackType type, CallbackHandler callback) const noexcept;
		bool areCallbacksRegistered(CallbackType type) const noexcept;
		bool areCallbacksRegistered() const noexcept;

	private:
		static asmjit::TypeId getTypeId(DataType type);

		std::weak_ptr<asmjit::JitRuntime> m_rt;
		std::array<std::vector<CallbackHandler>, 2> m_callbacks;
		std::shared_mutex m_mutex;
		uint64_t m_functionPtr = 0;
#if PLH_SOURCEHOOK
		uint64_t m_function2Ptr = 0;
#endif
		union {
			uint64_t m_trampolinePtr = 0;
			const char* m_errorCode;
		};

		std::unique_ptr<std::unordered_map<std::thread::id, std::deque<std::string>>> m_storage;
	};
}

inline PLH::ReturnFlag operator|(PLH::ReturnFlag lhs, PLH::ReturnFlag rhs) noexcept {
	using underlying = std::underlying_type_t<PLH::ReturnFlag>;
	return static_cast<PLH::ReturnFlag>(static_cast<underlying>(lhs) | static_cast<underlying>(rhs));
}

inline bool operator&(PLH::ReturnFlag lhs, PLH::ReturnFlag rhs) noexcept {
	using underlying = std::underlying_type_t<PLH::ReturnFlag>;
	return static_cast<underlying>(lhs) & static_cast<underlying>(rhs);
}

inline PLH::ReturnFlag& operator|=(PLH::ReturnFlag& lhs, PLH::ReturnFlag rhs) noexcept {
	lhs = lhs | rhs;
	return lhs;
}