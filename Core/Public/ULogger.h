#pragma once

#include "CoreDefine.h"
#include "UObject.h"

SPACE_H(Core)

class FWorkerLogChannel;

// 공용 큐와 워커 채널이 같은 묶음 기준을 사용한다.
// 묶음은 이 건수에 도달하거나 첫 로그 후 이 시간이 지나면 출력 대상으로 넘긴다.
STATIC_CONSTEXPR uint64 LOG_MAX_BATCH_RECORDS{ 64 };
STATIC_CONSTEXPR MILLISECONDS LOG_MAX_BATCH_WAIT{ 100 };

enum class ELogLevel : uint8
{
	Info, Warning, Error
};

struct FLogRecord
{
	NO_COPY(FLogRecord)
	FLogRecord(ELogLevel level, STRINGVIEW message)
		: OccurredAtTime(SYSTEM_CLOCK::now())
		, OrderTime(STEADY_CLOCK::now())
		, ProducerThreadID(std::this_thread::get_id())
		, Level(level)
		, Message(message)
	{}

	TIMEPOINT_SYSTEM_CLOCK OccurredAtTime;
	TIMEPOINT_STEADY_CLOCK OrderTime;
	THREAD_ID ProducerThreadID;
	ELogLevel Level;
	uint64 QueueSequenceNum{};
	FString Message;
};

using LOGBATCH_CONTAINER = VECTOR<SHPTR<FLogRecord>>;

// 한 워커 채널의 로그 레코드들을 전달하는 묶음이다.
struct FLogBatch
{
	uint64 ChannelID{};
	LOGBATCH_CONTAINER Records;
};

class CORE_DLL ULogger final : public UObject {
public:
	explicit ULogger(SHPTR<UCoreInstance> coreInstance);
	virtual ~ULogger() override;
	NO_COPY(ULogger)

	// 등록·핸들러 설정·Start와 RequestStop은 동시에 호출하지 않는다.
	// 제어 스레드에서 Start 전 호출한다. 채널 ID가 중복되면 실패한다.
	// Logger와 채널은 생산자와 로그 소비자가 끝날 때까지 유지해야 한다.
	SHPTR<FWorkerLogChannel> RegisterWorkerChannel(uint64 channelID) noexcept;

	// 채널 묶음의 출력 함수를 Start 전에 제어 스레드에서 설정한다.
	// 묶음 참조는 콜백이 끝날 때까지만 유효하다. 콜백이 예외를 던지면 Logger가 멈추고
	// 그 예외를 Start의 실패 콜백으로 전달한다.
	bool SetWorkerBatchHandler(FUNC<void(const FLogBatch&)> onBatch) noexcept;

	// 제어 스레드에서 워커 시작 전에 호출한다. 실패하면 접수를 시작하지 않는다.
	// 등록된 워커 채널이 있으면 SetWorkerBatchHandler가 먼저 성공해야 한다.
	// 묶음 핸들러는 로그 스레드에서 호출되며 보관할 로그의 포인터는 직접 소유해야 한다.
	// 실패 콜백은 오류가 발생한 스레드에서 호출되며 예외를 던지면 안 된다. 던지면 프로그램이 종료된다.
	bool Start(FUNC<void(const LOGBATCH_CONTAINER&)> onBatch, FUNC<void(EXPECTION_PTR)> onFailure);
	// true는 큐 접수 성공이다. 출력 및 post 성공을 뜻하지 않는다.
	// 내부 오류 시 Logger를 중단하며, 큐에 접수된 뒤 post가 실패해도 true를 반환한다.
	bool TryLog(ELogLevel level, STRINGVIEW message);
	// 시스템 오류 한 건을 "where (category:code): message" 형식의 Error 로그로 접수한다.
	// 반환값은 TryLog와 같다. 오류 문장을 만들 메모리가 없으면 Logger 오류로 보고 Logger를 멈춘다.
	bool TryLogError(STRINGVIEW where, const BOOST_ERROR_CODE& error) noexcept;
	// ThreadManager에 등록한다. Logger와 콜백 대상은 워커가 끝날 때까지 살아 있어야 한다.
	static void RunWorker(void* data) noexcept;
	// 모든 생산자의 TryLog와 채널 콜백이 끝난 뒤 호출한다. 스레드를 기다리지 않는다.
	void RequestStop() noexcept;

private:
	// 등록·핸들러 설정·Start는 Start 시도 전, 종료 요청 전에만 허용한다.
	bool IsConfigurable() const noexcept;
	// 워커 채널 알림은 생산자 스레드에서 게시만 수행한다.
	static bool NotifyWorkerChannel(void* context) noexcept;
	bool ScheduleWorkerCollect() noexcept;
	void CollectWorkerChannels(bool finalPass);
	void ArmWorkerTimer(TIMEPOINT_STEADY_CLOCK deadline);
	void OnWorkerTimer(const BOOST_ERROR_CODE& error, uint64 generation);
	void ArmBatchTimer();
	void OnBatchTimer(const BOOST_ERROR_CODE& error, uint64 generation);
	void FlushBatch();
	void Collect();
	void StopOnFailure(EXPECTION_PTR failure) noexcept;

	struct FChannelRegistration
	{
		uint64 ChannelID;
		SHPTR<FWorkerLogChannel> Channel;
	};

	VECTOR<FChannelRegistration> _WorkerChannels;
	CONQUEUE<SHPTR<FLogRecord>> _LogQueue;
	IOCONTEXT _LogContext;
	WORK_GUARD<IOCONTEXT::executor_type> _WorkGuard;
	BOOST_STEADY_TIMER _BatchTimer;
	BOOST_STEADY_TIMER _WorkerTimer;
	LOGBATCH_CONTAINER _Batch;
	uint64 _BatchGeneration{ 0 };
	uint64 _WorkerTimerGeneration{ 0 };
	TIMEPOINT_STEADY_CLOCK _WorkerDeadline{};
	bool _WorkerTimerArmed{ false };
	bool _StartAttempted{ false };
	FUNC<void(const FLogBatch&)> _OnWorkerBatch;
	FUNC<void(const LOGBATCH_CONTAINER&)> _OnBatch;
	FUNC<void(EXPECTION_PTR)> _OnFailure;
	ATOMIC_UINT64 _WorkerSignalSequence{ 0 };
	ATOMIC_BOOL _WorkerCollectPending{ false };
	// RequestStop 또는 소멸자가 켠다. RequestStop은 모든 생산자가 끝난 뒤 호출하므로 생산 종료도 뜻한다.
	ATOMIC_BOOL _StopRequested{ false };
	ATOMIC_BOOL _Accepting{ false };
	ATOMIC_BOOL _Failed{ false };
	uint64 _NextSequence{ 0 };
};

SPACE_T
