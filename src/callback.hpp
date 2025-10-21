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
#include <span>
#include <shared_mutex>
#include <map>
#include <thread>

#include <plg/any.hpp>
#include <plg/inplace_vector.hpp>
#include <plg/hybrid_vector.hpp>

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
		String,
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
		Post,  ///< Callback will be executed after the original function
		Count
	};

	enum class ReturnFlag : uint8_t {
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

		static constexpr size_t kMaxFuncStack = 62; // for 512 byte object

		using CallbackEntry = void (*)(Callback* callback, const Parameters* params, size_t count, const Return* ret, ReturnFlag* flag);
		using CallbackHandler = ReturnAction (*)(Callback* callback, const Parameters* params, int32_t count, const Return* ret, CallbackType type);

		Callback(DataType returnType, std::span<const DataType> arguments);
		~Callback();

		uint64_t getJitFunc(const asmjit::FuncSignature& sig, CallbackEntry pre, CallbackEntry post);
		uint64_t getJitFunc(DataType retType, std::span<const DataType> paramTypes, CallbackEntry pre, CallbackEntry post, uint8_t vaIndex);

		uint64_t* getTrampolineHolder() noexcept;
		uint64_t* getFunctionHolder() noexcept;
		plg::hybrid_vector<CallbackHandler, kMaxFuncStack> getCallbacks(CallbackType type) noexcept;
		std::string_view getError() const noexcept;

		plg::any& setStorage(size_t idx, const plg::any& any) const;
		plg::any& getStorage(size_t idx) const;

		DataType getReturnType() const;
		DataType getArgumentType(size_t idx) const;

		bool addCallback(CallbackType type, CallbackHandler callback, int priority = 0);
		bool removeCallback(CallbackType type, CallbackHandler callback);
		bool isCallbackRegistered(CallbackType type, CallbackHandler callback) const noexcept;
		bool areCallbacksRegistered(CallbackType type) const noexcept;
		bool areCallbacksRegistered() const noexcept;

	private:
		static asmjit::TypeId getTypeId(DataType type) noexcept;
		static bool hasHiArgSlot(const asmjit::x86::Compiler& compiler, asmjit::TypeId typeId) noexcept;
		uint64_t m_functionPtr = 0;
		union {
			uint64_t m_trampolinePtr = 0;
			const char* m_errorCode;
		};
		mutable std::shared_mutex m_mutex;
		std::inplace_vector<DataType, asmjit::Globals::kMaxFuncArgs> m_arguments;
		DataType m_returnType;
		struct CallbackObject {
			alignas(std::hardware_destructive_interference_size)
			plg::hybrid_vector<CallbackHandler, kMaxFuncStack> callbacks;
			plg::hybrid_vector<int, kMaxFuncStack> priorities;
		};
		std::array<CallbackObject, static_cast<size_t>(CallbackType::Count)> m_callbacks;
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