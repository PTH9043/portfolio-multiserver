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
		metricIndex.store(0);
	}
}

void UMPRuntimeMetrics::RecordMPMetrics(EMemoryPoolMetricIndex eMemoryPoolMetricIndex)
{
	// 색인 타입이 부호 없는 열거형이므로 상한만 확인한다.
	if(eMemoryPoolMetricIndex < EMemoryPoolMetricIndex::MaxIndex)
	{
		_MpMetricsIndexArray[eMemoryPoolMetricIndex].fetch_add(1);
	}
}

FMPRuntimeMetricsSnapshot UMPRuntimeMetrics::ReadSnapshot() const
{
	return FMPRuntimeMetricsSnapshot{
		.ThreadCacheHitCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryThreadCacheHit].load(),
		.CentralQueueHitCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryCentralQueueHit].load(),
		.SystemAllocationCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemorySystemAllocation].load(),
		.ReturnedToThreadCacheCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryReturnedToThreadCache].load(),
		.ReturnedToCentralQueueCount = _MpMetricsIndexArray[EMemoryPoolMetricIndex::MemoryReturnedToCentralQueue].load()
	};
}

SPACE_T

SPACE_T

#endif