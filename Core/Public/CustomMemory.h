#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

#include <concurrent_queue.h>

// 예제 전용 타입을 새로 만들지 않고 현재 Core의 타입과 매크로를 그대로 사용한다.
#include "CoreTypes.h"
#include "CoreMacro.h"

SPACE_H(Core)

class UMemoryHeader;
class UMemoryPool;

// 메모리 풀을 몇개 만들지, 어디까지 풀로 처리할지 결정
// PoolCount 합은 80입니다
struct FMemoryLayout final
{
	STATIC_CONSTEXPR uint64 PoolCount{
		(512 / 16) + (1024 / 32) + (1024 / 128) + (2048 / 256)
	};
	STATIC_CONSTEXPR uint64 MaxPooledAllocationSize{ 4608 };
	// 중앙 풀 버킷 하나가 재사용 대기열에 유지할 최대 총 바이트다. 초과 반납분은 OS로 즉시 반환해 피크 고착을 막는다.
	STATIC_CONSTEXPR uint64 MaxCentralCachedBytesPerPool{ 4ull * 1024ull * 1024ull };
	STATIC_CONSTEXPR uint64 MinCentralCachedBlocksPerPool{ 64 };
};

// Worker 하나가 반환된 block을 무제한 보관하지 못하도록 제한합니다.
struct FThreadCachePolicy final
{
	// 한 스레드가 캐쉬에 저장할 블록의 합 : 512KB
	STATIC_CONSTEXPR uint64 MaxCachedBytesPerThread{ 512 * 1024 };
	// 같은 크기의 블록은 최대 64개만 저장
	STATIC_CONSTEXPR uint64 MaxCachedBlocksPerPool{ 64 };
	// 블록 전체 크기가 2304바이트보다 크면 캐쉬에 저장하지 않는다. 
	STATIC_CONSTEXPR uint64 MaxThreadCachedBlockSize{ 2304 };
};

/*
-------------------------------
UMemoryHeader
-------------------------------
*/

// x64 malloc의 16바이트 정렬을 사용자 페이로드 주소(mHeader + 1)까지 그대로 보존한다.
class ALIGNAS(16) UMemoryHeader final
{
public:
	explicit UMemoryHeader(uint64 allocSize = 0);

	// 새 헤더 공간을 할당하지는 않고, 이미 존재하는 헤더에 크기를 기록함.
	static void* RecordHeader(UMemoryHeader* mHeader, uint64 allocSize)
	{
		RETURN_INVALID_COND(nullptr == mHeader, nullptr);

		mHeader->SetAllocSize(allocSize);
		// 현재 메모리에서 한 칸 뒤로 가서 사용자 주소를 반환한다.
		return reinterpret_cast<void*>(mHeader + 1);
	}

	// 사용자 주소에서 헤더 한 개만큼 뒤로 가서 관리 메모리를 찾음.
	static UMemoryHeader* DetachHeader(void* ptr)
	{
		return reinterpret_cast<UMemoryHeader*>(ptr) - 1;
	}

	// get set 
	void SetAllocSize(const uint64 allocSize) { _allocSize.store(allocSize); }
	uint64 GetAllocSize() const { return _allocSize.load(); }

	// AllocSize 초기화를 시도한다.
	bool TryRelease(REF_OUT uint64& allocSize) 
	{
		allocSize = _allocSize.exchange(0);
		return 0 != allocSize;
	}

private:
	ATOMIC_UINT64 _allocSize;
	// 16바이트 정렬 패딩을 명시해 /W4 C4324 경고를 방지한다.
	uint64 _padding{ 0 };
};

// 현재 헤더의 크기를 기록한다.
STATIC_CONSTEXPR uint64 MEMORYHEADER_SIZE{ sizeof(UMemoryHeader) };
// 헤더가 16바이트 정렬과 std::max_align_t 배수 조건을 모두 만족하는지 컴파일 시점에 확인한다.
static_assert(16 == sizeof(UMemoryHeader));
static_assert(0 == sizeof(UMemoryHeader) % alignof(std::max_align_t));

/*
-------------------------------
UMemoryHeader
------------------------------
UMemoryPool
------------------------------
*/

// 여러 worker가 함께 사용하는 중앙 free-block queue다.
class UMemoryPool final
{
	// 여기에 저장하는 건 블록의 주소, 메모리 헤더를 보관한다. 
public:
	explicit UMemoryPool(uint64 blockSize, uint32 poolIndex);
	~UMemoryPool();
	NO_COPY(UMemoryPool);

	// 다 쓴 블록 주소를 보관한다. 
	void Push(UMemoryHeader* pMemoryHeader) noexcept;
	// 블록 주소를 꺼내온다. 
	UMemoryHeader* Pop();

	const uint64 GetBlockSize() const { return _BlockSize; }
	const uint32 GetPoolIndex() const { return _PoolIndex;}
private:
	uint64			_BlockSize;
	uint32			_PoolIndex;
	uint64			_MaxCentralBlocks{ 0 };
	ATOMIC_UINT64	_QueuedBlockCount{ 0 };
	concurrency::concurrent_queue<UMemoryHeader*> _CentralQueue;
};

/*
-------------------------------
UMemoryPool
------------------------------
UThreadMemoryCache
------------------------------
*/

// 컴퓨터 CPU의 캐쉬 메모리와 같은 것
using CACHED_BLOCKS = std::array<UMemoryHeader*, 
	FThreadCachePolicy::MaxCachedBlocksPerPool>;

struct FPoolCacheSlot final
{
	UMemoryPool*	pMemoryPool{ nullptr };
	CACHED_BLOCKS	CachedBlocks{};
	uint64			CachedBlockIndex{ 0 };
};

class UThreadMemoryCache final
{
public:
	UThreadMemoryCache() = default;
	NO_COPY(UThreadMemoryCache)
	~UThreadMemoryCache();

	UMemoryHeader* TryPop(UMemoryPool* pMemoryPool);
	bool TryPush(UMemoryPool* pMemoryPool,  UMemoryHeader* pMemoryHeader);

	void FlushToCentral();
private:
	std::array<FPoolCacheSlot, FMemoryLayout::PoolCount> _PoolCacheSlots{};
	uint64				_TotalCachedBytes{ 0 };
};

extern thread_local UThreadMemoryCache GThreadMemoryCache;

/*
-------------------------------
UThreadMemoryCache
------------------------------
UMemoryAdiminstor
------------------------------
*/

// 현재 Core의 이름을 그대로 사용해 어느 위치에 적용되는지 바로 비교할 수 있게 한다.
class UMemoryAdiminstor final
{
public:
	UMemoryAdiminstor();
	~UMemoryAdiminstor();
	NO_COPY(UMemoryAdiminstor)

	void* Allocate(uint64 size);
	void Release(void* ptr);
private:
	void MakeMemoryPool(REF_OUT uint64& nextBlockSize, REF_OUT uint32& nextPoolIndex,
		REF_OUT uint32& nextLookupIndex, uint32 exclusiveUpperBound, uint32 blockSizeStep);
private:
	std::array<UMemoryPool*, FMemoryLayout::MaxPooledAllocationSize + 1> _PoolTable{};
	std::array<UQPTR<UMemoryPool>, FMemoryLayout::PoolCount> _MemoryTable{};
};

SPACE_T
