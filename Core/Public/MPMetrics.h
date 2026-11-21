#pragma once

#if defined(ENABLE_RUNTIME_METRICS) && defined(ENABLE_MP_RUNTIME_METRICS)
#include "RunTimeMetrics.h"

SPACE_H(Core)

SPACE_H(Metrics)

// MemoryPool이 필요한 측정 항목과 저장 방식을 직접 소유한다.
// 다른 subsystem은 IRuntimeMetrics만 상속하고 자기 방식으로 구현하면 된다.
class UMPRuntimeMetrics final : public IRuntimeMetrics
{
public:
	STRINGVIEW GetName() const override;
	void Reset() override;

	void RecordMPMetrics(EMemoryPoolMetricIndex eMemoryPoolMetricIndex);
	FMPRuntimeMetricsSnapshot ReadSnapshot() const;
private:
	// 항목별 원자 카운터를 64바이트 캐시라인으로 분리해 워커 간 거짓 공유(False Sharing)를 방지한다.
	struct ALIGNAS(64) FAlignedMetricCounter final
	{
		ATOMIC_UINT64 Value{ 0 };
		uint8 Padding[64 - sizeof(ATOMIC_UINT64)]{};
	};
	std::array<FAlignedMetricCounter, EMemoryPoolMetricIndex::MaxIndex> _MpMetricsIndexArray{};
};
SPACE_T

SPACE_T

#endif