#pragma once

#include <cstddef>
#include <limits>
#include <new>

SPACE_H(Core)

SPACE_H(Alloc)

class CORE_DLL UAllocator {
public:
	static void* Alloc(uint64 size);
	static void Release(void* ptr);
};

template<class T>
class USTLAllocator {
public:
	static_assert(alignof(T) <= alignof(std::max_align_t),
		"USTLAllocator requires fundamental alignment; use a separate aligned allocator for this type");
	using value_type = T;

	USTLAllocator() {}

	template<class Other>
	USTLAllocator(const USTLAllocator<Other>&) {}

	T* allocate(uint64 count)
	{
		// 곱셈 후 검사하면 이미 wraparound한 작은 요청을 할당할 수 있다.
		if (count > std::numeric_limits<std::size_t>::max() / sizeof(T))
			throw std::bad_array_new_length{};
		const uint64 size = count * sizeof(T);
		return static_cast<T*>(UAllocator::Alloc(size));
	}
	void deallocate(T* ptr, uint64)
	{
		UAllocator::Release(ptr);
	}

	template<typename U>
	bool operator==(const USTLAllocator<U>&) { return true; }

	template<typename U>
	bool operator!=(const USTLAllocator<U>&) { return false; }
};

SPACE_T


template<class T, class... Args>
SHPTR<T> MakeShared(Args&&... args)
{
	return std::allocate_shared<T>(
		Alloc::USTLAllocator<T>{},
		std::forward<Args>(args)...
	);
}

SPACE_T
