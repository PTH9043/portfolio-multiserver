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

void URuntimeMetricsManager::ResetAll()
{
	EXCLUSIVE_LOCK_GUARD lock{ _MetricsLock };
	for (const std::unique_ptr<IRuntimeMetrics>& metrics : _MetricsContainer)
	{
		metrics->Reset();
	}
}

uint64 URuntimeMetricsManager::GetRegisteredCount() const
{
	// 측정 객체는 생성자에서만 등록된다.
	return static_cast<uint64>(_MetricsContainer.size());
}

#if defined(ENABLE_MP_RUNTIME_METRICS)
/* MpRunTimeMetrics의 함수 */
void URuntimeMetricsManager::RecordMPMetrics(EMemoryPoolMetricIndex eMemoryPoolMetricIndex)
{
	RETURN_INVALID_COND(_MpRuntimeMetrics == nullptr, );
	_MpRuntimeMetrics->RecordMPMetrics(eMemoryPoolMetricIndex);
}

FMPRuntimeMetricsSnapshot URuntimeMetricsManager::ReadSnapshot() const
{	
	RETURN_INVALID_COND(_MpRuntimeMetrics == nullptr, {});
	return _MpRuntimeMetrics->ReadSnapshot();
}
#endif

SPACE_T

SPACE_T

#endif