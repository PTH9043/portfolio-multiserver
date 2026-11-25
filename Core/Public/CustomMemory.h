#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>

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
	// 중앙 풀 버킷 하나가 재사용 대기열에 유지할 최대 총 바이트와 링 버퍼 슬롯 상한이다. 초과 반납분은 OS로 즉시 반환해 피크 고착을 막는다.
	STATIC_CONSTEXPR uint64 MaxCentralCachedBytesPerPool{ 4ull * 1024ull * 1024ull };
	STATIC_CONSTEXPR uint64 MinCentralCachedBlocksPerPool{ 64 };
	STATIC_CONSTEXPR uint64 MaxCentralRingSlotsPerPool{ 2048 };
};

// Worker 하나가 반환된 block을 무제한 보관하지 못하도록 제한합니다.
struct FThreadCachePolicy final
{
	// 한 스레드가 캐시에 저장할 블록의 합 : 1MB
	STATIC_CONSTEXPR uint64 MaxCachedBytesPerThread{ 1024ull * 1024ull };
	// 블록 크기 구간별 슬롯 상한 및 중앙 풀 배치 교환 개수
	STATIC_CONSTEXPR uint64 SmallBlockMaxSlots{ 64 };
	STATIC_CONSTEXPR uint64 SmallBlockBatchCount{ 32 };
	STATIC_CONSTEXPR uint64 MediumBlockMaxSlots{ 32 };
	STATIC_CONSTEXPR uint64 MediumBlockBatchCount{ 16 };
	STATIC_CONSTEXPR uint64 LargeBlockMaxSlots{ 16 };
	STATIC_CONSTEXPR uint64 LargeBlockBatchCount{ 8 };
	STATIC_CONSTEXPR uint64 ExtraLargeBlockMaxSlots{ 8 };
	STATIC_CONSTEXPR uint64 ExtraLargeBlockBatchCount{ 4 };
	// 풀 최대 크기(4608B)까지 스레드 캐시를 허용해 2305~4608B 중앙 직행 절벽을 제거한다.
	STATIC_CONSTEXPR uint64 MaxThreadCachedBlockSize{ FMemoryLayout::MaxPooledAllocationSize };

	static constexpr uint64 GetMaxSlots(const uint64 blockSize) noexcept
	{
		if (blockSize <= 256)
			return SmallBlockMaxSlots;
		if (blockSize <= 1024)
			return MediumBlockMaxSlots;
		if (blockSize <= 2304)
			return LargeBlockMaxSlots;
		return ExtraLargeBlockMaxSlots;
	}

	static constexpr uint64 GetBatchCount(const uint64 blockSize) noexcept
	{
		if (blockSize <= 256)
			return SmallBlockBatchCount;
		if (blockSize <= 1024)
			return MediumBlockBatchCount;
		if (blockSize <= 2304)
			return LargeBlockBatchCount;
		return ExtraLargeBlockBatchCount;
	}
};

/*
-------------------------------
UMemoryHeader
-------------------------------
*/

// x64 malloc의 16바이트 정렬을 사용자 페이로드 주소(mHeader + 1)까지 그대로 보존한다.
// 기존 8바이트 정렬 패딩 자리를 자유 리스트용 침입형 _Next 포인터로 재사용해 추가 메모리 없이 배치 연결을 구성한다.
class ALIGNAS(16) UMemoryHeader final
{
public:
	explicit UMemoryHeader(uint64 allocSize = 0);

	// 새 헤더 공간을 할당하지는 않고, 이미 존재하는 헤더에 크기를 기록함.
	static void* RecordHeader(UMemoryHeader* mHeader, uint64 allocSize) noexcept
	{
		RETURN_INVALID_COND(nullptr == mHeader, nullptr);

		mHeader->SetNext(nullptr);
		mHeader->SetAllocSize(allocSize);
		// 현재 메모리에서 한 칸 뒤로 가서 사용자 주소를 반환한다.
		return reinterpret_cast<void*>(mHeader + 1);
	}

	// 사용자 주소에서 헤더 한 개만큼 뒤로 가서 관리 메모리를 찾음.
	static UMemoryHeader* DetachHeader(void* ptr) noexcept
	{
		return reinterpret_cast<UMemoryHeader*>(ptr) - 1;
	}

	// 할당 직후 단독 소유 구간이므로 relaxed store로 불필요한 seq_cst 배리어를 제거한다.
	void SetAllocSize(const uint64 allocSize) noexcept { _allocSize.store(allocSize, std::memory_order_relaxed); }
	uint64 GetAllocSize() const noexcept { return _allocSize.load(std::memory_order_relaxed); }

	UMemoryHeader* GetNext() const noexcept { return _Next; }
	void SetNext(UMemoryHeader* next) noexcept { _Next = next; }

	// AllocSize 초기화를 시도한다. acq_rel 교환으로 이중 해제를 차단하고 재사용 전 가시성을 보장한다.
	bool TryRelease(REF_OUT uint64& allocSize) noexcept
	{
		allocSize = _allocSize.exchange(0, std::memory_order_acq_rel);
		return 0 != allocSize;
	}

private:
	ATOMIC_UINT64 _allocSize;
	// 자유 리스트(TLS/중앙 배치)에 있을 때만 사용하는 침입형 링크 포인터(기존 8B 패딩 재사용).
	UMemoryHeader* _Next{ nullptr };
};

// 현재 헤더의 크기를 기록한다.
STATIC_CONSTEXPR uint64 MEMORYHEADER_SIZE{ sizeof(UMemoryHeader) };
// 헤더가 16바이트 정렬과 std::max_align_t 배수 조건을 모두 만족하는지 컴파일 시점에 확인한다.
static_assert(16 == sizeof(UMemoryHeader));
static_assert(0 == sizeof(UMemoryHeader) % alignof(std::max_align_t));

/*
-------------------------------
FMemoryBatch
-------------------------------
*/

// 침입형 _Next 포인터로 연결된 블록 묶음이다. Head·Tail 포인터 교체만으로 O(1) 배치 이동이 가능하다.
struct FMemoryBatch final
{
	UMemoryHeader* Head{ nullptr };
	UMemoryHeader* Tail{ nullptr };
	uint64 Count{ 0 };
};

/*
-------------------------------
UMemoryPool
-------------------------------
*/

// 여러 worker가 함께 사용하는 Vyukov Bounded MPMC Lock-Free 중앙 배치 풀이다.
// 64비트 단조 증가 시퀀스 + 영구 수명 고정 링 슬롯 배열로 ABA·UAF 없이 mutex 0개로 배치를 교환하며,
// 생산자(_EnqueuePos)·소비자(_DequeuePos)·각 슬롯을 64바이트 캐시 라인 정렬해 False Sharing을 차단한다.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif
class ALIGNAS(64) UMemoryPool final
{
public:
	explicit UMemoryPool(uint64 blockSize, uint32 poolIndex);
	~UMemoryPool();
	NO_COPY(UMemoryPool);

	// 다 쓴 블록 1개를 보관한다.
	void Push(UMemoryHeader* pMemoryHeader) noexcept;
	// 다 쓴 블록 묶음을 Lock-Free 링 버퍼에 O(1) 보관한다. 가득 차면 잠금 없이 OS로 즉시 반환한다.
	void PushBatch(FMemoryBatch batch) noexcept;
	// 블록 1개를 꺼내온다. 중앙 풀이 비어 있으면 OS에서 새로 할당한다.
	UMemoryHeader* Pop();
	// 중앙 풀에서 Lock-Free로 블록 묶음을 O(1) 꺼내온다. 비어 있으면 빈 배치를 반환한다.
	FMemoryBatch PopBatch() noexcept;
	// 중앙 풀이 비어 있을 때 OS에서 새 블록 1개를 할당한다.
	UMemoryHeader* AllocateFreshBlock() const;

	uint64 GetBlockSize() const noexcept { return _BlockSize; }
	uint32 GetPoolIndex() const noexcept { return _PoolIndex; }
	uint64 GetBatchCount() const noexcept { return _BatchCount; }
private:
	struct ALIGNAS(64) FRingSlot final
	{
		ATOMIC_UINT64 Sequence{ 0 };
		FMemoryBatch Batch{};
	};
	static_assert(ATOMIC_UINT64::is_always_lock_free, "Vyukov MPMC ring requires lock-free 64-bit atomics");

	static void FreeChain(UMemoryHeader* head) noexcept;
private:
	uint64 _BlockSize{ 0 };
	uint32 _PoolIndex{ 0 };
	uint64 _BatchCount{ 0 };
	uint64 _MaxCentralBlocks{ 0 };
	uint64 _Capacity{ 0 };
	uint64 _Mask{ 0 };
	// 생성 시 1회 확보한 2의 거듭제곱 크기 링 슬롯 배열(런타임 힙 할당 0회, PushBatch/PopBatch noexcept 보장).
	UQPTR<FRingSlot[]> _RingSlots{};
	ALIGNAS(64) ATOMIC_UINT64 _EnqueuePos{ 0 };
	ALIGNAS(64) ATOMIC_UINT64 _DequeuePos{ 0 };
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif

/*
-------------------------------
UThreadMemoryCache
-------------------------------
*/

// 스레드 로컬 캐시 슬롯(24바이트 고정: 80개 슬롯 전체가 ~1.9KB로 L1D 캐시에 상주).
struct FPoolCacheSlot final
{
	UMemoryPool* pMemoryPool{ nullptr };
	UMemoryHeader* Head{ nullptr };
	uint64 Count{ 0 };
};

class UThreadMemoryCache final
{
public:
	UThreadMemoryCache() = default;
	NO_COPY(UThreadMemoryCache)
	~UThreadMemoryCache();

	// 스레드 로컬 슬롯에서 O(1)으로 블록 1개를 꺼낸다. 비어 있으면 nullptr을 반환한다.
	UMemoryHeader* TryPop(UMemoryPool* pMemoryPool) noexcept;
	// 중앙 풀에서 가져온 배치의 나머지 블록을 스레드 로컬 슬롯에 한 번에 채운다.
	void RefillFromBatch(UMemoryPool* pMemoryPool, FMemoryBatch batch) noexcept;
	// 블록 1개를 스레드 로컬 슬롯에 반환한다. 슬롯 상한이나 스레드 총량 상한을 넘으면 배치 단위로 중앙 풀에 반납한다.
	bool TryPush(UMemoryPool* pMemoryPool, UMemoryHeader* pMemoryHeader) noexcept;

	void FlushToCentral() noexcept;
private:
	FMemoryBatch DetachBatchFromSlot(FPoolCacheSlot& cacheSlot, uint64 blockSize, uint64 countToDetach) noexcept;
	void EvictIfOverBudget(UMemoryPool* preferredPool) noexcept;
private:
	std::array<FPoolCacheSlot, FMemoryLayout::PoolCount> _PoolCacheSlots{};
	uint64 _TotalCachedBytes{ 0 };
};

extern thread_local UThreadMemoryCache GThreadMemoryCache;

/*
-------------------------------
UMemoryAdiminstor
-------------------------------
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

	// 0..MaxPooledAllocationSize(4608) 크기를 36KB 테이블 없이 산술 계산으로 풀 인덱스(0..79)로 변환한다.
	static constexpr uint32 ResolvePoolIndex(uint64 allocSize) noexcept
	{
		if (allocSize <= 496)
			return (0 == allocSize) ? 0u : static_cast<uint32>((allocSize - 1) >> 4);
		if (allocSize <= 1504)
			return 31u + static_cast<uint32>((allocSize - 481) >> 5);
		if (allocSize <= 2432)
			return 63u + static_cast<uint32>((allocSize - 1409) >> 7);
		return 71u + static_cast<uint32>((allocSize - 2305) >> 8);
	}
private:
	void MakeMemoryPool(REF_OUT uint64& nextBlockSize, REF_OUT uint32& nextPoolIndex,
		uint32 exclusiveUpperBound, uint32 blockSizeStep);
private:
	std::array<UQPTR<UMemoryPool>, FMemoryLayout::PoolCount> _MemoryTable{};
};

SPACE_T
