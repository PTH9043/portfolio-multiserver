#include "CoreDefine.h"
#include "CustomMemory.h"

#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <immintrin.h>
#endif

SPACE_H(Core)

namespace
{
	inline void CpuRelax() noexcept
	{
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
		_mm_pause();
#elif defined(__i386__) || defined(__x86_64__)
		__builtin_ia32_pause();
#elif defined(__aarch64__) || defined(_M_ARM64)
		__yield();
#else
		std::this_thread::yield();
#endif
	}

	// 실측 B1 정책: 12 -> 24 -> 48회 pause로 코어 간 캐시라인 전송 시간(~100ns)을 확보해 MESI 핑퐁을 억제하고,
	// OS 선점 등 극한 경합(64회 초과) 시에만 yield로 무한 스핀을 방지한다.
	inline void BackoffOnContention(uint32& step) noexcept
	{
		if (64 <= step)
		{
			std::this_thread::yield();
			return;
		}

		const uint32 pauseCount{ (0 == step) ? 12u : ((1 == step) ? 24u : 48u) };
		for (uint32 index{ 0 }; index < pauseCount; ++index)
		{
			CpuRelax();
		}
		++step;
	}
}

UMemoryHeader::UMemoryHeader(const uint64 allocSize) :
	_allocSize{ allocSize }
{
}

/*
-------------------------------
UMemoryPool
-------------------------------
*/

UMemoryPool::UMemoryPool(const uint64 blockSize, const uint32 poolIndex) :
	_BlockSize{ blockSize },
	_PoolIndex{ poolIndex },
	_BatchCount{ FThreadCachePolicy::GetBatchCount(blockSize) }
{
	if (MEMORYHEADER_SIZE > _BlockSize)
	{
		throw std::invalid_argument{ "pool block is smaller than UMemoryHeader" };
	}

	const uint64 byBytes{ FMemoryLayout::MaxCentralCachedBytesPerPool / _BlockSize };
	_MaxCentralBlocks = (byBytes > FMemoryLayout::MinCentralCachedBlocksPerPool)
		? byBytes
		: FMemoryLayout::MinCentralCachedBlocksPerPool;
	uint64 neededSlots{ (_MaxCentralBlocks + _BatchCount - 1) / _BatchCount };
	if (neededSlots > FMemoryLayout::MaxCentralRingSlotsPerPool)
	{
		neededSlots = FMemoryLayout::MaxCentralRingSlotsPerPool;
	}
	uint64 capacity{ 2 };
	while (capacity < neededSlots)
	{
		capacity <<= 1;
	}
	_Capacity = capacity;
	_Mask = capacity - 1;
	_RingSlots = std::make_unique<FRingSlot[]>(_Capacity);
	for (uint64 index{ 0 }; index < _Capacity; ++index)
	{
		_RingSlots[index].Sequence.store(index, std::memory_order_relaxed);
	}
}

UMemoryPool::~UMemoryPool()
{
	for (;;)
	{
		FMemoryBatch batch{ PopBatch() };
		if (nullptr == batch.Head)
		{
			break;
		}
		FreeChain(batch.Head);
	}
}

void UMemoryPool::Push(UMemoryHeader* pMemoryHeader) noexcept
{
	RETURN_INVALID_COND(nullptr == pMemoryHeader, );
	pMemoryHeader->SetNext(nullptr);
	PushBatch(FMemoryBatch{ pMemoryHeader, pMemoryHeader, 1 });
}

void UMemoryPool::PushBatch(FMemoryBatch batch) noexcept
{
	RETURN_INVALID_COND(nullptr == batch.Head || 0 == batch.Count, );
	batch.Tail->SetNext(nullptr);

	FRingSlot* slot{ nullptr };
	uint64 pos{ _EnqueuePos.load(std::memory_order_relaxed) };
	uint32 backoffStep{ 0 };
	for (;;)
	{
		slot = &_RingSlots[pos & _Mask];
		const uint64 seq{ slot->Sequence.load(std::memory_order_acquire) };
		const int64 dif{ static_cast<int64>(seq) - static_cast<int64>(pos) };
		if (0 == dif)
		{
			if (_EnqueuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
			{
				break;
			}
		}
		else if (0 > dif)
		{
			FreeChain(batch.Head);
			return;
		}
		else
		{
			pos = _EnqueuePos.load(std::memory_order_relaxed);
		}
		BackoffOnContention(backoffStep);
	}

	slot->Batch = batch;
	slot->Sequence.store(pos + 1, std::memory_order_release);
}

UMemoryHeader* UMemoryPool::Pop()
{
	FMemoryBatch batch{ PopBatch() };
	if (nullptr != batch.Head)
	{
		UMemoryHeader* result{ batch.Head };
		UMemoryHeader* remainder{ result->GetNext() };
		result->SetNext(nullptr);
		if (nullptr != remainder)
		{
			PushBatch(FMemoryBatch{ remainder, batch.Tail, batch.Count - 1 });
		}
		return result;
	}

	return AllocateFreshBlock();
}

FMemoryBatch UMemoryPool::PopBatch() noexcept
{
	FRingSlot* slot{ nullptr };
	uint64 pos{ _DequeuePos.load(std::memory_order_relaxed) };
	uint32 backoffStep{ 0 };
	for (;;)
	{
		slot = &_RingSlots[pos & _Mask];
		const uint64 seq{ slot->Sequence.load(std::memory_order_acquire) };
		const int64 dif{ static_cast<int64>(seq) - static_cast<int64>(pos + 1) };
		if (0 == dif)
		{
			if (_DequeuePos.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed))
			{
				break;
			}
		}
		else if (0 > dif)
		{
			return FMemoryBatch{};
		}
		else
		{
			pos = _DequeuePos.load(std::memory_order_relaxed);
		}
		BackoffOnContention(backoffStep);
	}

	FMemoryBatch batch{ slot->Batch };
	slot->Batch = FMemoryBatch{};
	slot->Sequence.store(pos + _Mask + 1, std::memory_order_release);
	return batch;
}

UMemoryHeader* UMemoryPool::AllocateFreshBlock() const
{
	void* memory{ std::malloc(static_cast<std::size_t>(_BlockSize)) };
	if (nullptr == memory)
	{
		throw std::bad_alloc{};
	}
	return ::new (memory) UMemoryHeader{};
}

void UMemoryPool::FreeChain(UMemoryHeader* head) noexcept
{
	while (nullptr != head)
	{
		UMemoryHeader* next{ head->GetNext() };
		std::free(head);
		head = next;
	}
}

/*
-------------------------------
UThreadMemoryCache
-------------------------------
*/

UThreadMemoryCache::~UThreadMemoryCache()
{
	// 정상 경로에서는 worker 종료 전에 FlushToCentral()을 호출한다.
	// 호출을 빠뜨렸다면 파괴된 UMemoryPool을 참조하지 않고 block을 직접 해제한다.
	for (FPoolCacheSlot& cacheSlot : _PoolCacheSlots)
	{
		UMemoryHeader* current{ cacheSlot.Head };
		while (nullptr != current)
		{
			UMemoryHeader* next{ current->GetNext() };
			std::free(current);
			current = next;
		}
		cacheSlot.Head = nullptr;
		cacheSlot.Count = 0;
		cacheSlot.pMemoryPool = nullptr;
	}
	_TotalCachedBytes = 0;
}

UMemoryHeader* UThreadMemoryCache::TryPop(UMemoryPool* pMemoryPool) noexcept
{
	RETURN_INVALID_COND(nullptr == pMemoryPool, nullptr);

	const uint64 poolIndex{ pMemoryPool->GetPoolIndex() };
	ASSERT_CRASH(poolIndex < FMemoryLayout::PoolCount);

	FPoolCacheSlot& cacheSlot{ _PoolCacheSlots[poolIndex] };
	// 이 풀에 연결된 칸이 아니거나 비어 있으면 캐시 미스다.
	RETURN_INVALID_COND(cacheSlot.pMemoryPool != pMemoryPool || nullptr == cacheSlot.Head, nullptr);

	UMemoryHeader* pMemoryHeader{ cacheSlot.Head };
	cacheSlot.Head = pMemoryHeader->GetNext();
	pMemoryHeader->SetNext(nullptr);
	--cacheSlot.Count;
	_TotalCachedBytes -= pMemoryPool->GetBlockSize();
	if (0 == cacheSlot.Count)
	{
		cacheSlot.pMemoryPool = nullptr;
	}
	return pMemoryHeader;
}

void UThreadMemoryCache::RefillFromBatch(UMemoryPool* pMemoryPool, FMemoryBatch batch) noexcept
{
	RETURN_INVALID_COND(nullptr == pMemoryPool || nullptr == batch.Head || 0 == batch.Count, );

	const uint64 blockSize{ pMemoryPool->GetBlockSize() };
	const uint64 poolIndex{ pMemoryPool->GetPoolIndex() };
	ASSERT_CRASH(poolIndex < _PoolCacheSlots.size());

	FPoolCacheSlot& cacheSlot{ _PoolCacheSlots[poolIndex] };
	if (nullptr != cacheSlot.pMemoryPool && cacheSlot.pMemoryPool != pMemoryPool)
	{
		pMemoryPool->PushBatch(batch);
		return;
	}

	cacheSlot.pMemoryPool = pMemoryPool;
	batch.Tail->SetNext(cacheSlot.Head);
	cacheSlot.Head = batch.Head;
	cacheSlot.Count += batch.Count;
	_TotalCachedBytes += blockSize * batch.Count;

	EvictIfOverBudget(pMemoryPool);
}

bool UThreadMemoryCache::TryPush(UMemoryPool* pMemoryPool, UMemoryHeader* pMemoryHeader) noexcept
{
	RETURN_INVALID_COND(nullptr == pMemoryPool || nullptr == pMemoryHeader, false);

	const uint64 blockSize{ pMemoryPool->GetBlockSize() };
	RETURN_INVALID_COND(FThreadCachePolicy::MaxThreadCachedBlockSize < blockSize, false);

	const uint64 poolIndex{ pMemoryPool->GetPoolIndex() };
	ASSERT_CRASH(poolIndex < _PoolCacheSlots.size());

	FPoolCacheSlot& cacheSlot{ _PoolCacheSlots[poolIndex] };
	if (nullptr == cacheSlot.pMemoryPool)
	{
		cacheSlot.pMemoryPool = pMemoryPool;
	}
	else if (cacheSlot.pMemoryPool != pMemoryPool)
	{
		return false;
	}

	pMemoryHeader->SetNext(cacheSlot.Head);
	cacheSlot.Head = pMemoryHeader;
	++cacheSlot.Count;
	_TotalCachedBytes += blockSize;

	const uint64 maxSlots{ FThreadCachePolicy::GetMaxSlots(blockSize) };
	if (cacheSlot.Count >= maxSlots)
	{
		const uint64 batchCount{ pMemoryPool->GetBatchCount() };
		FMemoryBatch toDrain{ DetachBatchFromSlot(cacheSlot, blockSize, batchCount) };
		pMemoryPool->PushBatch(toDrain);
	}

	if (_TotalCachedBytes > FThreadCachePolicy::MaxCachedBytesPerThread)
	{
		EvictIfOverBudget(pMemoryPool);
	}

	return true;
}

void UThreadMemoryCache::FlushToCentral() noexcept
{
	// 스레드 캐시를 비우고, 각 풀에 있는 블록을 배치 단위로 중앙 풀에 반환한다.
	for (FPoolCacheSlot& cacheSlot : _PoolCacheSlots)
	{
		UMemoryPool* pMemoryPool{ cacheSlot.pMemoryPool };
		CONTINUE_COND(nullptr == pMemoryPool);

		const uint64 blockSize{ pMemoryPool->GetBlockSize() };
		const uint64 batchCount{ pMemoryPool->GetBatchCount() };
		while (0 < cacheSlot.Count)
		{
			const uint64 take{ (cacheSlot.Count > batchCount) ? batchCount : cacheSlot.Count };
			FMemoryBatch batch{ DetachBatchFromSlot(cacheSlot, blockSize, take) };
			pMemoryPool->PushBatch(batch);
		}
		cacheSlot.pMemoryPool = nullptr;
	}
	ASSERT_CRASH(0 == _TotalCachedBytes);
}

FMemoryBatch UThreadMemoryCache::DetachBatchFromSlot(
	FPoolCacheSlot& cacheSlot,
	const uint64 blockSize,
	const uint64 countToDetach) noexcept
{
	FMemoryBatch batch{};
	if (nullptr == cacheSlot.Head || 0 == cacheSlot.Count || 0 == countToDetach)
	{
		return batch;
	}

	const uint64 actualCount{ (countToDetach < cacheSlot.Count) ? countToDetach : cacheSlot.Count };
	UMemoryHeader* head{ cacheSlot.Head };
	UMemoryHeader* tail{ head };
	for (uint64 index{ 1 }; index < actualCount; ++index)
	{
		tail = tail->GetNext();
	}

	cacheSlot.Head = tail->GetNext();
	tail->SetNext(nullptr);
	cacheSlot.Count -= actualCount;
	_TotalCachedBytes -= blockSize * actualCount;
	if (0 == cacheSlot.Count)
	{
		cacheSlot.pMemoryPool = nullptr;
	}

	batch.Head = head;
	batch.Tail = tail;
	batch.Count = actualCount;
	return batch;
}

void UThreadMemoryCache::EvictIfOverBudget(UMemoryPool* preferredPool) noexcept
{
	// 총량 상한을 넘기면 큰 블록 슬롯부터 배치 반납해 소형 핫 블록 기아를 방지한다.
	for (uint64 reverseIndex{ FMemoryLayout::PoolCount };
		reverseIndex > 0 && _TotalCachedBytes > FThreadCachePolicy::MaxCachedBytesPerThread;
		--reverseIndex)
	{
		FPoolCacheSlot& slot{ _PoolCacheSlots[reverseIndex - 1] };
		if (nullptr == slot.pMemoryPool || 0 == slot.Count)
		{
			continue;
		}
		if (slot.pMemoryPool == preferredPool && slot.Count <= 1)
		{
			continue;
		}

		UMemoryPool* pool{ slot.pMemoryPool };
		const uint64 blockSize{ pool->GetBlockSize() };
		const uint64 batchCount{ pool->GetBatchCount() };
		const uint64 maxDetach{ (pool == preferredPool) ? (slot.Count - 1) : slot.Count };
		const uint64 take{ (maxDetach > batchCount) ? batchCount : maxDetach };
		FMemoryBatch batch{ DetachBatchFromSlot(slot, blockSize, take) };
		pool->PushBatch(batch);
	}
}

thread_local UThreadMemoryCache GThreadMemoryCache;

/*
-------------------------------
UMemoryAdiminstor
-------------------------------
*/

UMemoryAdiminstor::UMemoryAdiminstor()
{
	uint32 nextPoolIndex{ 0 };
	uint64 nextBlockSize{ 16 };

	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex, 512, 16);
	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex, 1536, 32);
	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex, 2560, 128);
	MakeMemoryPool(REF_OUT nextBlockSize, REF_OUT nextPoolIndex,
		static_cast<uint32>(FMemoryLayout::MaxPooledAllocationSize + 1), 256);

	if (FMemoryLayout::PoolCount != nextPoolIndex)
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

		pMemoryHeader = ::new (memory) UMemoryHeader{};
	}
	else
	{
		UMemoryPool* pMemoryPool{ _MemoryTable[ResolvePoolIndex(allocSize)].get() };
		pMemoryHeader = GThreadMemoryCache.TryPop(pMemoryPool);
		if (nullptr == pMemoryHeader)
		{
			FMemoryBatch batch{ pMemoryPool->PopBatch() };
			if (nullptr != batch.Head)
			{
				pMemoryHeader = batch.Head;
				UMemoryHeader* remainderHead{ pMemoryHeader->GetNext() };
				pMemoryHeader->SetNext(nullptr);
				if (nullptr != remainderHead)
				{
					FMemoryBatch remainder{ remainderHead, batch.Tail, batch.Count - 1 };
					GThreadMemoryCache.RefillFromBatch(pMemoryPool, remainder);
				}
			}
			else
			{
				pMemoryHeader = pMemoryPool->AllocateFreshBlock();
			}
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

	UMemoryPool* pMemoryPool{ _MemoryTable[ResolvePoolIndex(allocSize)].get() };
	if (true == GThreadMemoryCache.TryPush(pMemoryPool, pMemoryHeader))
	{
		return;
	}
	pMemoryPool->Push(pMemoryHeader);
}

void UMemoryAdiminstor::MakeMemoryPool(
	REF_OUT uint64& nextBlockSize,
	REF_OUT uint32& nextPoolIndex,
	const uint32 exclusiveUpperBound,
	const uint32 blockSizeStep)
{
	while (exclusiveUpperBound > nextBlockSize)
	{
		THROW_INVALID_COND(FMemoryLayout::PoolCount <= nextPoolIndex,
			std::out_of_range{ "memory pool table is out of range" });

		_MemoryTable[nextPoolIndex] = std::make_unique<UMemoryPool>(nextBlockSize, nextPoolIndex);
		++nextPoolIndex;
		nextBlockSize += blockSizeStep;
	}
}

SPACE_T
