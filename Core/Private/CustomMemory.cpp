#include "CoreDefine.h"
#include "CustomMemory.h"

#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>

SPACE_H(Core)

UMemoryHeader::UMemoryHeader(const uint64 allocSize)  :
	_allocSize{ allocSize }
{}

/*
-------------------------------
UMemoryHeader
------------------------------
UMemoryPool
------------------------------
*/

UMemoryPool::UMemoryPool(const uint64 blockSize, const uint32 poolIndex) :
	_BlockSize{ blockSize },
	_PoolIndex{ poolIndex }
{
	if (MEMORYHEADER_SIZE > _BlockSize)
	{
		throw std::invalid_argument{ "pool block is smaller than UMemoryHeader" };
	}

	const uint64 byBytes{ FMemoryLayout::MaxCentralCachedBytesPerPool / _BlockSize };
	_MaxCentralBlocks = (byBytes > FMemoryLayout::MinCentralCachedBlocksPerPool)
		? byBytes
		: FMemoryLayout::MinCentralCachedBlocksPerPool;
}

UMemoryPool::~UMemoryPool() 
{
	UMemoryHeader* pMemoryHeader{ nullptr };
	while (_CentralQueue.try_pop(pMemoryHeader))
	{
		std::free(pMemoryHeader);
	}
	_QueuedBlockCount.store(0, std::memory_order_relaxed);
}

void UMemoryPool::Push(UMemoryHeader* pMemoryHeader) noexcept
{ 
	RETURN_INVALID_COND(nullptr == pMemoryHeader, );
	// 버킷 상한을 넘긴 여유 블록은 중앙 큐에 무한 보관하지 않고 OS로 즉시 반환한다.
	if (_QueuedBlockCount.load(std::memory_order_relaxed) >= _MaxCentralBlocks)
	{
		std::free(pMemoryHeader);
		return;
	}
	_QueuedBlockCount.fetch_add(1, std::memory_order_relaxed);
	_CentralQueue.push(pMemoryHeader);
}

UMemoryHeader* UMemoryPool::Pop()
{
	// 블록 크기가 헤더보다 작은 풀은 생성자에서 거부된다.
	UMemoryHeader* mHeader{ nullptr };
	if (_CentralQueue.try_pop(mHeader))
	{
		_QueuedBlockCount.fetch_sub(1, std::memory_order_relaxed);
		// MemoryCentralQueueHit는 중앙 큐에서 블록을 가져온 횟수
		RECORD_RUNTIME_METRIC(Metrics::EMemoryPoolMetricIndex::MemoryCentralQueueHit);
		return mHeader;
	}

	void* memory{ std::malloc(static_cast<std::size_t>(_BlockSize)) };
	if (nullptr == memory)
	{
		throw std::bad_alloc{};
	}
	// MemorySystemAllocation은 스레드 캐시와 중앙 큐를 거치지 않고 시스템에서 직접 할당한 횟수
	RECORD_RUNTIME_METRIC(Metrics::EMemoryPoolMetricIndex::MemorySystemAllocation);
	return ::new (memory) UMemoryHeader{};
}

/*
-------------------------------
UMemoryPool
------------------------------
UThreadMemoryCache
------------------------------
*/

UThreadMemoryCache::~UThreadMemoryCache() 
{
	// 정상 경로에서는 worker 종료 전에 FlushToCentral()을 호출한다.
	// 호출을 빠뜨렸다면 파괴된 UMemoryPool을 참조하지 않고 block을 직접 해제한다.
	for (FPoolCacheSlot& cacheSlot : _PoolCacheSlots)
	{
		for (uint64 index{ 0 }; index < cacheSlot.CachedBlockIndex; ++index)
		{
			std::free(cacheSlot.CachedBlocks[index]);
		}
	}
}

UMemoryHeader* UThreadMemoryCache::TryPop(UMemoryPool* pMemoryPool)
{
	RETURN_INVALID_COND(nullptr == pMemoryPool, nullptr);

	uint64 PoolIndex{ pMemoryPool->GetPoolIndex() };
	ASSERT_CRASH(PoolIndex < FMemoryLayout::PoolCount);

	FPoolCacheSlot& CacheSlot{ _PoolCacheSlots[PoolIndex] };
	// 이 풀에 연결된 칸이 아니거나 아직연결 전, 다른 관리자의 풀이 비어있으면 캐시미스
	RETURN_INVALID_COND(CacheSlot.pMemoryPool != pMemoryPool || 0 == CacheSlot.CachedBlockIndex, nullptr);

	// 현재 슬롯을 하나 감소 하고 메모리를 꺼낸다.
	--CacheSlot.CachedBlockIndex;
	UMemoryHeader* pMemoryHeader{ CacheSlot.CachedBlocks[CacheSlot.CachedBlockIndex] };
	// 캐쉬 슬롯을 비우고 나서 현재 TotalBytes를 감소시킨다. 
	CacheSlot.CachedBlocks[CacheSlot.CachedBlockIndex] = nullptr;
	_TotalCachedBytes -= pMemoryPool->GetBlockSize();
	return pMemoryHeader;
}

bool UThreadMemoryCache::TryPush(UMemoryPool* pMemoryPool,UMemoryHeader* pMemoryHeader)
{
	RETURN_INVALID_COND(nullptr == pMemoryPool || nullptr == pMemoryHeader, false);

	uint64 BlockSize{ pMemoryPool->GetBlockSize() };
	// 1차로 현재 BlockSize가 MaxThreadCachedBlockSize보다 큰지 확인한다.
	RETURN_INVALID_COND(FThreadCachePolicy::MaxThreadCachedBlockSize < BlockSize, false);
	// 2차로 현재 ThreadCache에 저장된 총 블록의 크기가 MaxCachedBytesPerThread보다 큰지 확인한다.
	RETURN_INVALID_COND(FThreadCachePolicy::MaxCachedBytesPerThread - _TotalCachedBytes < BlockSize, false);

	
	uint64 PoolIndex{ pMemoryPool->GetPoolIndex() };
	ASSERT_CRASH(PoolIndex < _PoolCacheSlots.size());

	FPoolCacheSlot& CacheSlot{ _PoolCacheSlots[PoolIndex] };
	RETURN_INVALID_COND(FThreadCachePolicy::MaxCachedBlocksPerPool <= CacheSlot.CachedBlockIndex, false);

	// 빈 칸이면 이 풀의 칸이라고 이름표를 붙인다. 다른 풀 칸이면 멈춘다.
	if (nullptr == CacheSlot.pMemoryPool)
		CacheSlot.pMemoryPool = pMemoryPool;

	ASSERT_CRASH(CacheSlot.pMemoryPool == pMemoryPool);

	CacheSlot.CachedBlocks[CacheSlot.CachedBlockIndex] = pMemoryHeader;
	++CacheSlot.CachedBlockIndex;
	_TotalCachedBytes += BlockSize;
	return true;
}

void UThreadMemoryCache::FlushToCentral()
{
	// 스레드 캐시를 비우고, 각 풀에 있는 블록을 중앙 큐로 반환한다.
	for (FPoolCacheSlot& CacheSlot : _PoolCacheSlots)
	{
		UMemoryPool* pMemoryPool{ CacheSlot.pMemoryPool };
		CONTINUE_COND(nullptr == pMemoryPool);

		const uint64 BlockSize{ pMemoryPool->GetBlockSize() };
		// 현재 슬롯이 비어있지 않을 때
		while (0 < CacheSlot.CachedBlockIndex)
		{
			// 슬롯의 블록을 꺼내서 중앙 큐로 반환한다.
			--CacheSlot.CachedBlockIndex;
			UMemoryHeader* pMemoryHeader{ CacheSlot.CachedBlocks[CacheSlot.CachedBlockIndex] };
			CacheSlot.CachedBlocks[CacheSlot.CachedBlockIndex] = nullptr;
			_TotalCachedBytes -= BlockSize;
			pMemoryPool->Push(pMemoryHeader);
		}

		CacheSlot.pMemoryPool = nullptr;
	}
	ASSERT_CRASH(0 == _TotalCachedBytes);
}

thread_local UThreadMemoryCache GThreadMemoryCache;

/*
-------------------------------
UThreadMemoryCache
------------------------------
UMemoryAdiminstor
------------------------------
*/

UMemoryAdiminstor::UMemoryAdiminstor()
{
	uint32 nextLookupIndex{ 0 };
	uint32 nextPoolIndex{ 0 };
	uint64 nextBlockSize{ 16 };

	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex, 
		REF_OUT nextLookupIndex, 512, 16);
	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex,
		REF_OUT nextLookupIndex, 1536, 32);
	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex,
		REF_OUT nextLookupIndex, 2560, 128);
	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex,
		REF_OUT nextLookupIndex, static_cast<uint32>(FMemoryLayout::MaxPooledAllocationSize + 1), 256);

	if (FMemoryLayout::PoolCount != nextPoolIndex
		|| FMemoryLayout::MaxPooledAllocationSize + 1 != nextLookupIndex)
	{
		throw std::logic_error{ "memory pool table count mismatch" };
	}
}

UMemoryAdiminstor::~UMemoryAdiminstor()
{
	GThreadMemoryCache.FlushToCentral();
}

void* UMemoryAdiminstor::Allocate(const uint64 size)
{
	STATIC_CONSTEXPR uint64 MaxMemorySize{
		static_cast<uint64>(std::numeric_limits<std::size_t>::max())
	};

	THROW_INVALID_COND(MaxMemorySize - MEMORYHEADER_SIZE < size, 
		std::bad_array_new_length{});

	const uint64 allocSize{ size + MEMORYHEADER_SIZE };
	UMemoryHeader* pMemoryHeader{ nullptr };

	if (FMemoryLayout::MaxPooledAllocationSize < allocSize)
	{
		void* memory{ std::malloc(static_cast<std::size_t>(allocSize)) };
		THROW_INVALID_COND(nullptr == memory, std::bad_alloc{});

		// MemorySystemAllocation은 스레드 캐시와 중앙 큐를 거치지 않고 시스템에서 직접 할당한 횟수
		RECORD_RUNTIME_METRIC(Metrics::EMemoryPoolMetricIndex::MemorySystemAllocation);

		pMemoryHeader = ::new (memory) UMemoryHeader{};
	}
	else
	{
		UMemoryPool* pMemoryPool{ _PoolTable[allocSize] };
		pMemoryHeader = GThreadMemoryCache.TryPop(pMemoryPool);
		if (nullptr != pMemoryHeader)
		{
			// MemoryThreadCacheHit는 스레드 캐시에서 블록을 가져온 횟수
			RECORD_RUNTIME_METRIC(Metrics::EMemoryPoolMetricIndex::MemoryThreadCacheHit);
		}
		else
		{
			pMemoryHeader = pMemoryPool->Pop();
		}
	}

	return UMemoryHeader::RecordHeader(pMemoryHeader, allocSize);
}

void UMemoryAdiminstor::Release(void* ptr)
{
	RETURN_INVALID_COND(nullptr == ptr, );

	UMemoryHeader* pMemoryHeader{ UMemoryHeader::DetachHeader(ptr) };
	uint64 allocSize{ 0 };
	const bool firstRelease{ pMemoryHeader->TryRelease(REF_OUT allocSize) };
	ASSERT_CRASH(firstRelease);

	if (FMemoryLayout::MaxPooledAllocationSize < allocSize)
	{
		std::free(pMemoryHeader);
		return;
	}

	UMemoryPool* pMemoryPool{ _PoolTable[allocSize] };
	if (true == GThreadMemoryCache.TryPush(pMemoryPool, pMemoryHeader))
	{
		// MemoryReturnedToThreadCache는 스레드 캐시에 반환한 횟수
		RECORD_RUNTIME_METRIC(Metrics::EMemoryPoolMetricIndex::MemoryReturnedToThreadCache);
		return;
	}
	RECORD_RUNTIME_METRIC(Metrics::EMemoryPoolMetricIndex::MemoryReturnedToCentralQueue);
	pMemoryPool->Push(pMemoryHeader);
}

void UMemoryAdiminstor::MakeMemoryPool(
	REF_OUT uint64& nextBlockSize,
	REF_OUT uint32& nextPoolIndex,
	REF_OUT uint32& nextLookupIndex,
	const uint32 exclusiveUpperBound,
	const uint32 blockSizeStep)
{
	while (exclusiveUpperBound > nextBlockSize)
	{
		THROW_INVALID_COND(FMemoryLayout::PoolCount <= nextPoolIndex
			|| FMemoryLayout::MaxPooledAllocationSize < nextLookupIndex,
			std::out_of_range{ "memory pool table is out of range" });

		UQPTR<UMemoryPool> memoryPool{
			std::make_unique<UMemoryPool>(nextBlockSize, nextPoolIndex)
		};
		UMemoryPool* memoryPoolPointer{ memoryPool.get() };
		_MemoryTable[nextPoolIndex] = std::move(memoryPool);
		++nextPoolIndex;

		while (nextBlockSize >= nextLookupIndex)
		{
			_PoolTable[nextLookupIndex] = memoryPoolPointer;
			++nextLookupIndex;
		}

		nextBlockSize += blockSizeStep;
	}
}


SPACE_T
