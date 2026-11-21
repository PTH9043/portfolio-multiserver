#include "CoreDefine.h"

#if defined(ENABLE_RUNTIME_METRICS) && defined(ENABLE_MP_RUNTIME_METRICS)

#include "MPMetrics.h"


SPACE_H(Core)

SPACE_H(Metrics)

std::string_view UMPRuntimeMetrics::GetName() const
{
	return "MemoryPool";
}

void UMPRuntimeMetrics::Reset()
{
	for(auto& metricIndex : _MpMetricsIndexArray)
	{
		metricIndex.Value.store(0, std::memory_order_relaxed);
	}
}

void UMPRuntimeMetrics::RecordMPMetrics(EMemoryPoolMetricIndex eMemoryPoolMetricIndex)
{
	// 색인 타입이 부호 없는 열거형이므로 상한만 확인한다.
	if(eMemoryPoolMetricIndex < EMemoryPoolMetricIndex::MaxIndex)
	{
		_MpMetricsIndexArray[eMemoryPoolMetricIndex].Value.fetch_add(1, std::memory_order_relaxed);
	}
}

FMPRuntimeMetricsSnapshot UMPRuntimeMetrics::ReadSnapshot() const
{
	return FMPRuntimeMetricsSnapshot{
		.ThreadCacheHitCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryThreadCacheHit].Value.load(std::memory_order_relaxed),
		.CentralQueueHitCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryCentralQueueHit].Value.load(std::memory_order_relaxed),
		.SystemAllocationCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemorySystemAllocation].Value.load(std::memory_order_relaxed),
		.ReturnedToThreadCacheCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryReturnedToThreadCache].Value.load(std::memory_order_relaxed),
		.ReturnedToCentralQueueCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryReturnedToCentralQueue].Value.load(std::memory_order_relaxed)
	};
}

SPACE_T

SPACE_T

#endif