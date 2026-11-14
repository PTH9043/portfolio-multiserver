#pragma once

#if defined(ENABLE_RUNTIME_METRICS)

SPACE_H(Core)

SPACE_H(Metrics)

class UMPRuntimeMetrics;

/* MpRunTimeMetrics의 데이터 */
struct FMPRuntimeMetricsSnapshot final
{
	uint64 ThreadCacheHitCount{ 0 };
	uint64 CentralQueueHitCount{ 0 };
	uint64 SystemAllocationCount{ 0 };
	uint64 ReturnedToThreadCacheCount{ 0 };
	uint64 ReturnedToCentralQueueCount{ 0 };
};

enum EMemoryPoolMetricIndex : uint32
{
	MemoryThreadCacheHit,
	MemoryCentralQueueHit,
	MemorySystemAllocation,
	MemoryReturnedToThreadCache,
	MemoryReturnedToCentralQueue,
	MaxIndex
};


/*
static storage duration

직접 따옴표로 작성한 "Hello"는 스택 메모리가 아니라 정적 메모리로 유지됨

static storage duration의 규칙은
static, thread_local, new 그리고 어떤 함수에 적히고 어떤 리턴값으로 적히냐에 따라 메모리 유지시간 결정.

그래서 string_view가 유효한 이유는 "hello"라고 적혔을 때 -> 이 "hello"가 정적 메모리로 저장됨으로
해당 메모리를 읽고 가리키기만 하는 string_view가 더 유효함.
*/

// 매니저가 서로 다른 subsystem의 측정 객체를 동일한 방식으로 관리하기 위한
// 최소 인터페이스다. counter의 개수와 저장 방식은 자식 클래스가 결정한다.
class IRuntimeMetrics abstract
{
public:
	virtual ~IRuntimeMetrics() = default;

	virtual STRINGVIEW GetName() const = 0;
	virtual void Reset() = 0;
};

// 활성화된 측정 객체를 생성하고 수명과 공통 동작을 관리한다.
// 기록 경로에서는 이 매니저를 검색하지 않고 등록할 때 얻은 참조를 사용한다.
class CORE_DLL URuntimeMetricsManager final
{
public:
	URuntimeMetricsManager();
	~URuntimeMetricsManager();
	NO_COPY(URuntimeMetricsManager);

	void ResetAll();
	uint64 GetRegisteredCount() const;

#if defined(ENABLE_MP_RUNTIME_METRICS)
	/* MpRunTimeMetrics의 함수 */
	void RecordMPMetrics(EMemoryPoolMetricIndex eMemoryPoolMetricIndex);
	FMPRuntimeMetricsSnapshot ReadSnapshot() const;
#endif
private:
	mutable SHARED_MUTEX			_MetricsLock;
	std::vector<std::unique_ptr<IRuntimeMetrics>> _MetricsContainer;

#if defined(ENABLE_MP_RUNTIME_METRICS)
	UMPRuntimeMetrics* _MpRuntimeMetrics{ nullptr };
#endif
};

SPACE_T

SPACE_T

#endif

// 메모리 풀 코드에서 쓰는 기록 매크로. 두 계측 스위치 중 하나라도 꺼지면 아무 일도 하지 않는 문장이 된다.
// 스위치와 관계없이 정의되므로 호출하는 쪽에서 #if로 감싸지 않는다.
#if defined(ENABLE_RUNTIME_METRICS) && defined(ENABLE_MP_RUNTIME_METRICS)
#define RECORD_RUNTIME_METRIC(MetricIndex) \
	GCoreGlobal.GetRuntimeMetricsManager().RecordMPMetrics(MetricIndex)
#else
#define RECORD_RUNTIME_METRIC(MetricIndex) ((void)0)
#endif
