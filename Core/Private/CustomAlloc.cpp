#include "CoreDefine.h"
#include "CustomAlloc.h"

SPACE_H(Core)

SPACE_H(Alloc)

void* UAllocator::Alloc(uint64 size)
{
	return GMemory.Allocate(size);
}

void UAllocator::Release(void* ptr)
{
	GMemory.Release(ptr);
}

SPACE_T

SPACE_T