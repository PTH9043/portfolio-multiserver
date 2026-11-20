#include "CoreDefine.h"
#include "ULogger.h"
#include "FWorkerLogChannel.h"

#include <boost/asio/post.hpp>
#include <boost/system/system_error.hpp>
#include <exception>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/ULogger.cpp"

SPACE_H(Core)

ULogger::ULogger(SHPTR<UCoreInstance> coreInstance)
    : UObject{ coreInstance }
    , _WorkGuard{ ASIO::make_work_guard(_LogContext) }
    , _BatchTimer{ _LogContext }
    , _WorkerTimer{ _LogContext }
{
    RUNTIME_TRACE_EVENT(Create, ULogger, ULogger, GetRuntimeTraceID());
}

ULogger::~ULogger()
{
    RUNTIME_TRACE_SCOPE(ULogger, ~ULogger, GetRuntimeTraceID());
    // 소비자와 생산자가 끝난 후 파괴한다. 미시작 채널도 이후 알림을 보내지 않게 닫는다.
    _StopRequested.store(true, std::memory_order_release);
    for (const FChannelRegistration& registration : _WorkerChannels)
        registration.Channel->FinishProducing();
    RUNTIME_TRACE_EVENT(Destroy, ULogger, ~ULogger, GetRuntimeTraceID());
}

SHPTR<FWorkerLogChannel> ULogger::RegisterWorkerChannel(uint64 channelID) noexcept
{
    RUNTIME_TRACE_SCOPE(ULogger, RegisterWorkerChannel, GetRuntimeTraceID());
    // 등록과 Start는 워커 실행 전 제어 스레드에서 순차적으로 호출한다.
    if (!IsConfigurable())
        return {};

    for (const FChannelRegistration& registration : _WorkerChannels) {
        if (registration.ChannelID == channelID)
            return {};
    }

    try {
        SHPTR<FWorkerLogChannel> channel = MakeShared<FWorkerLogChannel>(channelID);
        if (!channel->SetNotifier(&ULogger::NotifyWorkerChannel, this)) {
            StopOnFailure(channel->GetNotificationState().Failure);
            return {};
        }
        _WorkerChannels.push_back(FChannelRegistration{ channelID, channel });
        return channel;
    }
    catch (...) {
        StopOnFailure(std::current_exception());
        return {};
    }
}

bool ULogger::SetWorkerBatchHandler(FUNC<void(const FLogBatch&)> onBatch) noexcept
{
    RUNTIME_TRACE_SCOPE(ULogger, SetWorkerBatchHandler, GetRuntimeTraceID());
    if (!IsConfigurable() || !onBatch)
        return false;
    _OnWorkerBatch = std::move(onBatch);
    return true;
}

bool ULogger::Start(FUNC<void(const LOGBATCH_CONTAINER&)> onBatch, FUNC<void(EXPECTION_PTR)> onFailure)
{
    RUNTIME_TRACE_SCOPE(ULogger, Start, GetRuntimeTraceID());
    if (!IsConfigurable() || !onBatch || !onFailure)
        return false;
    if (!_WorkerChannels.empty() && !_OnWorkerBatch)
        return false;
    _StartAttempted = true;

    try {
        _OnFailure = std::move(onFailure);
        _OnBatch = std::move(onBatch);
        _Batch.reserve(LOG_MAX_BATCH_RECORDS);
        _Accepting.store(true, std::memory_order_release);
        return true;
    }
    catch (...) {
        StopOnFailure(std::current_exception());
        return false;
    }
}

bool ULogger::TryLog(ELogLevel level, STRINGVIEW message)
{
    RUNTIME_TRACE_SCOPE(ULogger, TryLog, GetRuntimeTraceID());
    if (level == ELogLevel::Error)
        TriggerRuntimeTraceDump();
    if (!_Accepting.load(std::memory_order_acquire))
        return false;

    bool accepted = false;
    try {
        _LogQueue.push(MakeShared<FLogRecord>(level, message));
        accepted = true;
        const auto submission = RUNTIME_TRACE_EVENT(Submit, ULogger, TryLog, GetRuntimeTraceID());
        ASIO::post(_LogContext, [this, submission] {
            RUNTIME_TRACE_CALLBACK(ULogger, TryLog, GetRuntimeTraceID(), submission);
            Collect();
        });
    }
    catch (...) {
        StopOnFailure(std::current_exception());
    }
    return accepted;
}

bool ULogger::TryLogError(const STRINGVIEW where, const BOOST_ERROR_CODE& error) noexcept
{
    RUNTIME_TRACE_SCOPE(ULogger, TryLogError, GetRuntimeTraceID());
    // 접수하지 않을 때는 문장을 만들지 않는다.
    if (!_Accepting.load(std::memory_order_acquire))
        return false;

    try {
        FString message{ where };
        message << " (" << error.category().name() << ':' << std::to_string(error.value())
                << "): " << error.message();
        return TryLog(ELogLevel::Error, message.View());
    }
    catch (...) {
        StopOnFailure(std::current_exception());
        return false;
    }
}

void ULogger::RunWorker(void* data) noexcept
{
    ULogger* logger = static_cast<ULogger*>(data);
    RUNTIME_TRACE_SCOPE(ULogger, RunWorker, logger->GetRuntimeTraceID());
    try {
        // work guard가 있으므로 run()은 RequestStop이 게시한 작업이 guard를 놓거나, StopOnFailure가 멈추거나,
        // 처리 함수(출력 콜백 포함)의 예외가 빠져나올 때만 끝난다. 예외는 아래 catch가 받는다.
        logger->_LogContext.run();
        // 남은 로그를 마지막으로 수거한다. 실패로 멈췄으면 세 함수 모두 바로 반환한다.
        logger->Collect();
        logger->FlushBatch();
        logger->CollectWorkerChannels(true);
    }
    catch (...) {
        logger->StopOnFailure(std::current_exception());
    }
}

void ULogger::RequestStop() noexcept
{
    RUNTIME_TRACE_SCOPE(ULogger, RequestStop, GetRuntimeTraceID());
    // 호출자는 모든 생산자의 로그 접수가 완료됐음을 보장한다.
    if (_StopRequested.exchange(true, std::memory_order_acq_rel))
        return;

    _Accepting.store(false, std::memory_order_release);
    for (const FChannelRegistration& registration : _WorkerChannels)
        registration.Channel->FinishProducing();

    if (_Failed.load(std::memory_order_acquire))
        return;

    try {
        const auto submission = RUNTIME_TRACE_EVENT(Submit, ULogger, RequestStop, GetRuntimeTraceID());
        ASIO::post(_LogContext, [this, submission] {
            RUNTIME_TRACE_CALLBACK(ULogger, RequestStop, GetRuntimeTraceID(), submission);
            ++_BatchGeneration;
            _BatchTimer.cancel();
            ++_WorkerTimerGeneration;
            _WorkerTimerArmed = false;
            _WorkerTimer.cancel();
            _WorkGuard.reset();
        });
    }
    catch (...) {
        StopOnFailure(std::current_exception());
    }
}

bool ULogger::IsConfigurable() const noexcept
{
    return !_StartAttempted && !_StopRequested.load(std::memory_order_acquire) &&
        !_Failed.load(std::memory_order_acquire);
}

bool ULogger::NotifyWorkerChannel(void* context) noexcept
{
    ULogger* logger = static_cast<ULogger*>(context);
    if (logger->_StopRequested.load(std::memory_order_acquire) ||
        logger->_Failed.load(std::memory_order_acquire))
        return true;

    logger->_WorkerSignalSequence.fetch_add(1, std::memory_order_release);
    return logger->ScheduleWorkerCollect();
}

bool ULogger::ScheduleWorkerCollect() noexcept
{
    RUNTIME_TRACE_SCOPE(ULogger, ScheduleWorkerCollect, GetRuntimeTraceID());
    if (_WorkerCollectPending.exchange(true, std::memory_order_acq_rel))
        return true;
    try {
        const auto submission = RUNTIME_TRACE_EVENT(Submit, ULogger, ScheduleWorkerCollect, GetRuntimeTraceID());
        ASIO::post(_LogContext, [this, submission] {
            RUNTIME_TRACE_CALLBACK(ULogger, ScheduleWorkerCollect, GetRuntimeTraceID(), submission);
            const uint64 observed = _WorkerSignalSequence.load(std::memory_order_acquire);
            CollectWorkerChannels(false);
            // store가 아니라 exchange를 쓴다. 생산자의 exchange(true)와 순서가 정해져,
            // 수거 중에 들어온 알림을 아래 비교에서 놓치지 않는다.
            _WorkerCollectPending.exchange(false, std::memory_order_acq_rel);
            if (!_Failed.load(std::memory_order_acquire) &&
                _WorkerSignalSequence.load(std::memory_order_acquire) != observed)
                ScheduleWorkerCollect();
        });
        return true;
    }
    catch (...) {
        // Logger가 멈추면 수거 요청을 다시 보내지 않으므로 대기 표시는 되돌리지 않는다.
        StopOnFailure(std::current_exception());
        return false;
    }
}

void ULogger::CollectWorkerChannels(const bool finalPass)
{
    RUNTIME_TRACE_SCOPE(ULogger, CollectWorkerChannels, GetRuntimeTraceID());
    if (!_OnWorkerBatch || _Failed.load(std::memory_order_acquire))
        return;

    bool readyRemaining{};
    do {
        readyRemaining = false;
        bool hasDeadline{};
        TIMEPOINT_STEADY_CLOCK earliestDeadline{};
        const TIMEPOINT_STEADY_CLOCK now = STEADY_CLOCK::now();

        // 채널마다 한 번에 묶음 하나만 출력한다. 남은 묶음은 일반 수거에서는 다시 게시하고 최종 수거에서는 바로 반복한다.
        // 출력 중 생산자가 채널을 오류 상태로 바꾸면 그 알림이 다음 수거를 부르므로, 유효성은 채널마다 한 번만 확인한다.
        for (const FChannelRegistration& registration : _WorkerChannels) {
            FWorkerLogChannel& channel = *registration.Channel;
            FWorkerLogChannel::FNotificationState state = channel.GetNotificationState();
            // 최종 수거에는 작성 묶음이 없다. RequestStop의 FinishProducing이 이미 인계했고 이후 접수를 막는다.
            if (state.HasWritingBatch && now >= state.FirstRecordTime + LOG_MAX_BATCH_WAIT) {
                channel.PublishExpiredBatch(now);
                state = channel.GetNotificationState();
            }
            if (!state.Valid) {
                StopOnFailure(state.Failure);
                return;
            }

            if (state.HasReadyBatch) {
                // 준비된 묶음이 있는데 꺼내지 못하는 경우는 생산자가 채널을 오류 상태로 바꾼 때뿐이다.
                const FLogBatch* batch = channel.TryBeginOutput();
                if (!batch) {
                    StopOnFailure(channel.GetNotificationState().Failure);
                    return;
                }
                for (const SHPTR<FLogRecord>& record : batch->Records)
                    record->QueueSequenceNum = _NextSequence++;
                // 출력 콜백의 예외는 공용 큐의 FlushBatch와 같이 RunWorker의 catch가 받아 Logger를 멈춘다.
                _OnWorkerBatch(*batch);
                if (!channel.FinishOutput()) {
                    StopOnFailure(channel.GetNotificationState().Failure);
                    return;
                }
                state = channel.GetNotificationState();
            }

            if (state.HasReadyBatch)
                readyRemaining = true;
            if (state.HasWritingBatch) {
                const TIMEPOINT_STEADY_CLOCK deadline = state.FirstRecordTime + LOG_MAX_BATCH_WAIT;
                if (!hasDeadline || deadline < earliestDeadline) {
                    hasDeadline = true;
                    earliestDeadline = deadline;
                }
            }
        }

        if (!finalPass && hasDeadline)
            ArmWorkerTimer(earliestDeadline);
        if (!finalPass && readyRemaining) {
            _WorkerSignalSequence.fetch_add(1, std::memory_order_release);
            ScheduleWorkerCollect();
        }
    } while (finalPass && readyRemaining);
}

void ULogger::ArmWorkerTimer(const TIMEPOINT_STEADY_CLOCK deadline)
{
    RUNTIME_TRACE_SCOPE(ULogger, ArmWorkerTimer, GetRuntimeTraceID());
    if (_WorkerTimerArmed && _WorkerDeadline == deadline)
        return;
    const uint64 generation = ++_WorkerTimerGeneration;
    _WorkerTimer.expires_at(deadline);
    _WorkerDeadline = deadline;
    _WorkerTimerArmed = true;
    const auto submission = RUNTIME_TRACE_EVENT(Submit, ULogger, ArmWorkerTimer, GetRuntimeTraceID());
    _WorkerTimer.async_wait([this, generation, submission](const BOOST_ERROR_CODE& error) {
        RUNTIME_TRACE_CALLBACK(ULogger, ArmWorkerTimer, GetRuntimeTraceID(), submission);
        OnWorkerTimer(error, generation);
    });
}

void ULogger::OnWorkerTimer(const BOOST_ERROR_CODE& error, const uint64 generation)
{
    RUNTIME_TRACE_SCOPE(ULogger, OnWorkerTimer, GetRuntimeTraceID());
    // 세대가 다르면 다시 설정되기 전의 대기다.
    if (generation != _WorkerTimerGeneration || error == ASIO::error::operation_aborted)
        return;
    _WorkerTimerArmed = false;
    if (error) {
        StopOnFailure(std::make_exception_ptr(boost::system::system_error(error)));
        return;
    }
    CollectWorkerChannels(false);
}

void ULogger::ArmBatchTimer()
{
    RUNTIME_TRACE_SCOPE(ULogger, ArmBatchTimer, GetRuntimeTraceID());
    const uint64 generation = ++_BatchGeneration;
    _BatchTimer.expires_after(LOG_MAX_BATCH_WAIT);
    const auto submission = RUNTIME_TRACE_EVENT(Submit, ULogger, ArmBatchTimer, GetRuntimeTraceID());
    _BatchTimer.async_wait([this, generation, submission](const BOOST_ERROR_CODE& error) {
        RUNTIME_TRACE_CALLBACK(ULogger, ArmBatchTimer, GetRuntimeTraceID(), submission);
        OnBatchTimer(error, generation);
    });
}

void ULogger::OnBatchTimer(const BOOST_ERROR_CODE& error, uint64 generation)
{
    RUNTIME_TRACE_SCOPE(ULogger, OnBatchTimer, GetRuntimeTraceID());
    // 세대가 다르면 이미 출력한 묶음의 타이머다.
    if (generation != _BatchGeneration || error == ASIO::error::operation_aborted)
        return;
    if (error) {
        StopOnFailure(std::make_exception_ptr(boost::system::system_error(error)));
        return;
    }
    FlushBatch();
}

void ULogger::FlushBatch()
{
    RUNTIME_TRACE_SCOPE(ULogger, FlushBatch, GetRuntimeTraceID());
    if (_Failed.load(std::memory_order_acquire) || _Batch.empty())
        return;

    ++_BatchGeneration;
    _BatchTimer.cancel();
    // 출력 콜백의 예외는 run() 또는 마지막 수거에서 빠져나와 RunWorker의 catch가 Logger를 멈춘다.
    _OnBatch(_Batch);
    _Batch.clear();
}

void ULogger::Collect()
{
    RUNTIME_TRACE_SCOPE(ULogger, Collect, GetRuntimeTraceID());
    // 묶음은 64건이 되면 바로 출력하므로 여기 들어올 때는 항상 64건보다 적다.
    SHPTR<FLogRecord> record;
    while (!_Failed.load(std::memory_order_acquire) && _LogQueue.try_pop(record)) {
        record->QueueSequenceNum = _NextSequence++;
        const bool firstRecord = _Batch.empty();
        _Batch.push_back(std::move(record));
        if (_Batch.size() >= LOG_MAX_BATCH_RECORDS)
            FlushBatch();
        else if (firstRecord && !_StopRequested.load(std::memory_order_acquire))
            ArmBatchTimer();
    }
}

void ULogger::StopOnFailure(EXPECTION_PTR failure) noexcept
{
    RUNTIME_TRACE_SCOPE(ULogger, StopOnFailure, GetRuntimeTraceID());
    if (_Failed.exchange(true, std::memory_order_acq_rel))
        return;
    _Accepting.store(false, std::memory_order_release);
    _LogContext.stop();
    for (const FChannelRegistration& registration : _WorkerChannels)
        registration.Channel->FinishProducing();

    // 실패 콜백은 예외를 던지지 않아야 한다. 던지면 noexcept 경계에서 프로그램이 종료된다.
    if (_OnFailure)
        _OnFailure(failure);
}

SPACE_T
