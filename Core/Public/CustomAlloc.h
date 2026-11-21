#pragma once

#include <cstddef>
#include <limits>
#include <new>

SPACE_H(Core)

SPACE_H(Alloc)

// 전역 메모리 풀(GMemory)을 호출하는 저수준 할당·해제 진입점이다.
class CORE_DLL UAllocator {
public:
	static void* Alloc(uint64 size);
	static void Release(void* ptr);
};

// STL 컨테이너와 allocate_shared가 전역 메모리 풀을 쓰도록 연결하는 커스텀 할당기다.
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


// 전역 메모리 풀을 사용해 객체와 제어 블록을 한 번에 생성하는 shared_ptr 팩토리다.
template<class T, class... Args>
SHPTR<T> MakeShared(Args&&... args)
{
	return std::allocate_shared<T>(
		Alloc::USTLAllocator<T>{},
		std::forward<Args>(args)...
	);
}

SPACE_T
