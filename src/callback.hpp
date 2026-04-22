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
#include <plg/format.hpp>
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
		Post, ///< Callback will be executed after the original function
		Count,
	};

	enum class ReturnFlag : uint8_t {
		Default = 0, ///< Value means this gives no information about return flag.
		NoPost = 1,
		Supercede = 2,
	};

	class Callback {
	public:
		static constexpr size_t kMaxFuncStack = 62; // for 512 byte object

		using CallbackEntry = void (*)(Callback* callback, uint64_t* params, size_t count, /*uint128_t*/ void* ret, ReturnFlag* flag);
		using CallbackEntry2 = void (*)(Callback* callback, uintptr_t* params);
		using CallbackHandler = ReturnAction (*)(Callback* callback, void* params, int32_t count, void* ret, CallbackType type);

		Callback();
		Callback(DataType returnType, std::span<const DataType> arguments);
		~Callback();

		uint64_t getJitFunc(const asmjit::FuncSignature& sig, CallbackEntry pre, CallbackEntry post);
		uint64_t getJitFunc(DataType retType, std::span<const DataType> paramTypes, CallbackEntry pre, CallbackEntry post, uint8_t vaIndex);
		uint64_t getJitFunc(CallbackEntry2 entry); // for mid hook

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
		static void saveRegisters(asmjit::x86::Assembler& assembler);
		static void restoreRegisters(asmjit::x86::Assembler& assembler);
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
		struct alignas(std::hardware_constructive_interference_size) CallbackObject {
			plg::hybrid_vector<CallbackHandler, kMaxFuncStack> callbacks;
			plg::hybrid_vector<int, kMaxFuncStack> priorities;
		};
		std::array<CallbackObject, static_cast<size_t>(CallbackType::Count)> m_callbacks;
	};

	template <typename T>
	concept SlotStorable = std::is_trivially_copyable_v<T> && sizeof(T) <= sizeof(uint64_t);

	static constexpr size_t SizeOf(DataType type) noexcept {
		switch (type) {
			case DataType::Bool:
				return sizeof(bool);
			case DataType::Int8:
				return sizeof(int8_t);
			case DataType::Int16:
				return sizeof(int16_t);
			case DataType::Int32:
				return sizeof(int32_t);
			case DataType::Int64:
				return sizeof(int64_t);
			case DataType::UInt8:
				return sizeof(uint8_t);
			case DataType::UInt16:
				return sizeof(uint16_t);
			case DataType::UInt32:
				return sizeof(uint32_t);
			case DataType::UInt64:
				return sizeof(uint64_t);
			case DataType::Pointer:
				return sizeof(void*);
			case DataType::Float:
				return sizeof(float);
			case DataType::Double:
				return sizeof(double);
			case DataType::String:
				return sizeof(const char*);
			default:
				return 0;
		}
	}

	template <typename S>
	class ParametersSpan {
	public:
		using slot_type = S;

		ParametersSpan(slot_type* data, size_t count) noexcept
		    : _data(data)
		    , _count(count) {
		}

		template <typename T>
		    requires SlotStorable<T>
		T get(size_t index) const noexcept {
			assert(index < _count && "Index out of bounds");
			T result{};
			std::memcpy(&result, &_data[index], sizeof(T));
			return result;
		}

		template <typename T>
		    requires SlotStorable<T>
		void set(size_t index, const T& value) noexcept {
			assert(index < _count && "Index out of bounds");
			std::memcpy(&_data[index], &value, sizeof(T));
		}

	private:
		slot_type* _data;
		[[maybe_unused]] size_t _count;
	};

	class ReturnSlot {
	public:
		using type = void;

		explicit ReturnSlot(type* data, size_t size) noexcept
		    : _data(data)
		    , _size(size) {
		}

		template <typename T>
		    requires std::is_trivially_copyable_v<T>
		void set(const T& value) noexcept {
			assert(sizeof(T) <= _size && "Return value too large");
			std::memcpy(_data, &value, sizeof(T));
		}

		template <typename T>
		    requires std::is_trivially_copyable_v<T>
		T get() const noexcept {
			assert(sizeof(T) <= _size && "Return type too large");
			T result{};
			std::memcpy(&result, _data, sizeof(T));
			return result;
		}

		template <typename T, typename... Args>
			// requires std::is_trivially_destructible_v<T>
		void construct(Args&&... args) noexcept(noexcept(T(std::forward<Args>(args)...))) {
			assert(sizeof(T) <= _size && "Type too large");
			std::construct_at(reinterpret_cast<T*>(_data), std::forward<Args>(args)...);
		}

	private:
		type* _data;
		[[maybe_unused]] size_t _size;
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