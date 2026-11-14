#pragma once

#include "ULogger.h"

#include <list>

SPACE_H(Core)

// 제어 스레드가 채널을 만들고, 한 생산자와 한 로그 소비자가 사용한다.
// Logger는 생산자와 소비자가 모두 끝날 때까지 채널을 유지해야 한다.
class CORE_DLL FWorkerLogChannel final
{
public:
    // 콜백은 호출 스레드에서 잠금 없이 실행된다. false는 알림 요청 실패다.
    using FNotify = bool (*)(void*) noexcept;

    struct FNotificationState
    {
        bool Valid{};
        bool HasWritingBatch{};
        bool HasReadyBatch{};
        TIMEPOINT_STEADY_CLOCK FirstRecordTime{};
        EXPECTION_PTR Failure;
    };

    explicit FWorkerLogChannel(uint64 channelID) noexcept;
    ~FWorkerLogChannel() = default;
    NO_COPY(FWorkerLogChannel)

    // 생산 시작 전 한 번 설정한다. 콜백과 context는 마지막 호출이 끝날 때까지 유효해야 한다.
    bool SetNotifier(FNotify notify, void* context) noexcept;
    // 알림이 누락되거나 합쳐져도 현재 채널 상태와 첫 로그 시각을 조회한다.
    FNotificationState GetNotificationState() const noexcept;

    // true: 이 채널이 지역 묶음에 접수했다. 파일에 저장됐다는 뜻은 아니다.
    bool TryLog(ELogLevel level, STRINGVIEW message) noexcept;
    // 로그 소비자가 호출한다. true는 오래된 부분 묶음을 전달했다는 뜻이다.
    bool PublishExpiredBatch(TIMEPOINT_STEADY_CLOCK now) noexcept;
    // 생산이 끝났거나 Logger 오류로 접수를 중단할 때 호출한다.
    void FinishProducing() noexcept;
    // 소비자는 반환 포인터를 FinishOutput 전까지만 읽는다. 동시에 하나만 처리한다.
    const FLogBatch* TryBeginOutput() noexcept;
    // 출력을 마친 묶음을 해제하고 예약량을 돌려준다. 출력이 실패하면 Logger 전체가 멈추므로 호출하지 않는다.
    // false는 출력 중인 묶음이 없거나 해제 준비 중 할당이 실패해 채널이 오류 상태가 됐다는 뜻이다.
    bool FinishOutput() noexcept;

private:
    struct FEntry
    {
        FLogBatch Batch;
        uint64 ReservedBytes{};
        TIMEPOINT_STEADY_CLOCK FirstRecordTime{};
    };

    // 다음 두 함수는 _Mutex를 소유한 상태에서 호출한다.
    bool AppendRecord(SHPTR<FLogRecord>& record, FNotify& notify, void*& context) noexcept;
    void PrepareNotification(FNotify& notify, void*& context) noexcept;

    // 알림 호출은 _Mutex 밖에서 수행한다.
    void Notify(FNotify notify, void* context) noexcept;
    // _Mutex 밖에서 호출한다. 채널을 오류 상태로 바꾸고 Logger에 알리며, 처음 기록한 원인만 보관한다.
    void MarkFailure(EXPECTION_PTR failure) noexcept;

    // _Mutex 소유 상태에서 호출한다. 로그가 담긴 작성 묶음이 없으면 nullptr이다.
    const FEntry* GetWritingEntry() const noexcept;

    // _Mutex 소유 상태에서 호출한다. 인계 시 큐 노드를 새로 할당하지 않는다.
    void PublishWritingBatch() noexcept;

    // 묶음 건수와 대기 시간은 ULogger.h의 LOG_MAX_BATCH_RECORDS, LOG_MAX_BATCH_WAIT를 따른다.
    static constexpr uint64 ChannelBudget{ 1024ULL * 1024ULL };
    static constexpr uint64 MaxMessageBytes{ 4ULL * 1024ULL };

    const uint64 _ChannelID;
    mutable SHARED_MUTEX _Mutex;
    ATOMIC_BOOL _Failed{ false };
    EXPECTION_PTR _Failure;
    FNotify _Notifier{};
    void* _NotifyContext{};
    std::list<FEntry> _Writing;
    std::list<FEntry> _Ready;
    uint64 _ReservedBytes{};
    // SetNotifier가 생산 시작 전인지 확인하는 데 쓴다.
    uint64 _AcceptedCount{};
    bool _ProducingFinished{};
    bool _OutputInProgress{};
};

SPACE_T
