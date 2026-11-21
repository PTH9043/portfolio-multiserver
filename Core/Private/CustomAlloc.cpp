#include "CoreDefine.h"
#include "CustomAlloc.h"

SPACE_H(Core)

SPACE_H(Alloc)

// 전역 메모리 풀에서 요청 바이트 크기만큼 할당한다.
void* UAllocator::Alloc(uint64 size)
{
	return GMemory.Allocate(size);
}

// 메모리 풀에서 할당받은 블록을 반환한다.
void UAllocator::Release(void* ptr)
{
	GMemory.Release(ptr);
}

SPACE_T

SPACE_T