#include "CoreDefine.h"
#include "RunTimeMetrics.h"

#if defined(ENABLE_RUNTIME_METRICS)

#include "MPMetrics.h"

SPACE_H(Core)

SPACE_H(Metrics)

URuntimeMetricsManager::URuntimeMetricsManager()
{
#if defined(ENABLE_MP_RUNTIME_METRICS)
	auto metrics = std::make_unique<UMPRuntimeMetrics>();
	metrics->Reset();

	auto* registeredMetrics = metrics.get();
	_MetricsContainer.push_back(std::move(metrics));
	_MpRuntimeMetrics = registeredMetrics;
#endif
}

URuntimeMetricsManager::~URuntimeMetricsManager()
{}

// 등록된 모든 런타임 메트릭스 카운터를 잠금 하에 초기화한다.
void URuntimeMetricsManager::ResetAll()
{
	EXCLUSIVE_LOCK_GUARD lock{ _MetricsLock };
	for (const std::unique_ptr<IRuntimeMetrics>& metrics : _MetricsContainer)
	{
		metrics->Reset();
	}
}

// 등록된 메트릭스 수집기 개수를 돌려준다.
uint64 URuntimeMetricsManager::GetRegisteredCount() const
{
	// 측정 객체는 생성자에서만 등록된다.
	return static_cast<uint64>(_MetricsContainer.size());
}

#if defined(ENABLE_MP_RUNTIME_METRICS)
// 메모리 풀 메트릭스 이벤트를 기록한다.
void URuntimeMetricsManager::RecordMPMetrics(EMemoryPoolMetricIndex eMemoryPoolMetricIndex)
{
	RETURN_INVALID_COND(_MpRuntimeMetrics == nullptr, );
	_MpRuntimeMetrics->RecordMPMetrics(eMemoryPoolMetricIndex);
}

// 현재 메모리 풀 메트릭스 스냅샷을 조회한다.
FMPRuntimeMetricsSnapshot URuntimeMetricsManager::ReadSnapshot() const
{	
	RETURN_INVALID_COND(_MpRuntimeMetrics == nullptr, {});
	return _MpRuntimeMetrics->ReadSnapshot();
}
#endif

SPACE_T

SPACE_T

#endif