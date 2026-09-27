#ifndef KYTY_COMMON_UNIQUEFUNCTION_H_
#define KYTY_COMMON_UNIQUEFUNCTION_H_

#include <cstddef>
#include <new>
#include <type_traits>
#include <utility>

namespace Common {

// Move-only type-erased callable. Callables up to InlineSize bytes are stored in place: the GPU
// command thread defers thousands of small lambdas per second (end-of-pipe interrupts, image
// retirement), and a heap allocation for each showed up in its profile.
template <typename Result, typename... Args>
class UniqueFunction {
	static constexpr size_t InlineSize  = 48;
	static constexpr size_t InlineAlign = alignof(std::max_align_t);

	struct VTable {
		Result (*invoke)(void* storage, Args&&... args);
		void (*move)(void* destination, void* source) noexcept;
		void (*destroy)(void* storage) noexcept;
	};

	template <typename Function>
	static constexpr bool FitsInline = sizeof(Function) <= InlineSize &&
	                                   alignof(Function) <= InlineAlign &&
	                                   std::is_nothrow_move_constructible_v<Function>;

	template <typename Function>
	static const VTable* InlineTable() {
		static constexpr VTable table {
		    [](void* storage, Args&&... args) -> Result {
			    return (*std::launder(static_cast<Function*>(storage)))(std::forward<Args>(args)...);
		    },
		    [](void* destination, void* source) noexcept {
			    auto* function = std::launder(static_cast<Function*>(source));
			    ::new (destination) Function(std::move(*function));
			    function->~Function();
		    },
		    [](void* storage) noexcept { std::launder(static_cast<Function*>(storage))->~Function(); },
		};
		return &table;
	}

	template <typename Function>
	static const VTable* HeapTable() {
		static constexpr VTable table {
		    [](void* storage, Args&&... args) -> Result {
			    return (**static_cast<Function**>(storage))(std::forward<Args>(args)...);
		    },
		    [](void* destination, void* source) noexcept {
			    *static_cast<Function**>(destination) = *static_cast<Function**>(source);
		    },
		    [](void* storage) noexcept { delete *static_cast<Function**>(storage); },
		};
		return &table;
	}

public:
	UniqueFunction() = default;

	template <typename Function, typename Decayed = std::decay_t<Function>,
	          typename = std::enable_if_t<!std::is_same_v<Decayed, UniqueFunction>>>
	UniqueFunction(Function&& function) {
		if constexpr (FitsInline<Decayed>) {
			::new (static_cast<void*>(m_storage)) Decayed(std::forward<Function>(function));
			m_vtable = InlineTable<Decayed>();
		} else {
			*reinterpret_cast<Decayed**>(m_storage) = new Decayed(std::forward<Function>(function));
			m_vtable                                = HeapTable<Decayed>();
		}
	}

	UniqueFunction(UniqueFunction&& other) noexcept: m_vtable(other.m_vtable) {
		if (m_vtable != nullptr) {
			m_vtable->move(m_storage, other.m_storage);
			other.m_vtable = nullptr;
		}
	}

	UniqueFunction& operator=(UniqueFunction&& other) noexcept {
		if (this != &other) {
			Reset();
			m_vtable = other.m_vtable;
			if (m_vtable != nullptr) {
				m_vtable->move(m_storage, other.m_storage);
				other.m_vtable = nullptr;
			}
		}
		return *this;
	}

	UniqueFunction(const UniqueFunction&)            = delete;
	UniqueFunction& operator=(const UniqueFunction&) = delete;

	~UniqueFunction() { Reset(); }

	Result operator()(Args... args) const {
		return m_vtable->invoke(const_cast<unsigned char*>(m_storage), std::forward<Args>(args)...);
	}

	explicit operator bool() const noexcept { return m_vtable != nullptr; }

private:
	void Reset() noexcept {
		if (m_vtable != nullptr) {
			m_vtable->destroy(m_storage);
			m_vtable = nullptr;
		}
	}

	alignas(InlineAlign) unsigned char m_storage[InlineSize];
	const VTable* m_vtable = nullptr;
};

} // namespace Common

#endif // KYTY_COMMON_UNIQUEFUNCTION_H_
