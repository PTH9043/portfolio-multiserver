#include "CoreDefine.h"
#include "FWorkerLogChannel.h"
#include <exception>

SPACE_H(Core)

FWorkerLogChannel::FWorkerLogChannel(const uint64 channelID) noexcept
    : _ChannelID{ channelID }
{}

bool FWorkerLogChannel::SetNotifier(FNotify notify, void* context) noexcept
{
    if (!notify)
        return false;

    EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
    if (_Notifier || _AcceptedCount != 0 || _ProducingFinished)
        return false;
    _Notifier = notify;
    _NotifyContext = context;
    return true;
}

FWorkerLogChannel::FNotificationState FWorkerLogChannel::GetNotificationState() const noexcept
{
    SHARED_LOCK_GUARD lock{ _Mutex };
    FNotificationState state{};
    state.Valid = !_Failed.load(std::memory_order_acquire);
    state.Failure = _Failure;
    state.HasReadyBatch = !_Ready.empty();
    if (const FEntry* entry = GetWritingEntry()) {
        state.HasWritingBatch = true;
        state.FirstRecordTime = entry->FirstRecordTime;
    }
    return state;
}

bool FWorkerLogChannel::TryLog(const ELogLevel level, const STRINGVIEW message) noexcept
{
    if (_Failed.load(std::memory_order_acquire) || message.size() > MaxMessageBytes)
        return false;

    // 레코드와 메시지 문자열을 만드는 할당만 실패할 수 있다. 실패하면 채널을 오류 상태로 바꿔 Logger가 멈추게 한다.
    SHPTR<FLogRecord> record;
    try {
        record = MakeShared<FLogRecord>(level, message);
    }
    catch (...) {
        MarkFailure(std::current_exception());
        return false;
    }

    FNotify notify{};
    void* notifyContext{};
    bool accepted{};
    {
        EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
        if (_ProducingFinished || _Failed.load(std::memory_order_acquire))
            return false;
        accepted = AppendRecord(record, notify, notifyContext);
    }
    Notify(notify, notifyContext);
    return accepted;
}

bool FWorkerLogChannel::PublishExpiredBatch(const TIMEPOINT_STEADY_CLOCK now) noexcept
{
    EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
    const FEntry* entry{ GetWritingEntry() };
    if (!entry)
        return false;
    if (now < entry->FirstRecordTime || now - entry->FirstRecordTime < LOG_MAX_BATCH_WAIT)
        return false;
    PublishWritingBatch();
    return true;
}

void FWorkerLogChannel::FinishProducing() noexcept
{
    FNotify notify{};
    void* notifyContext{};
    {
        EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
        if (!_ProducingFinished) {
            _ProducingFinished = true;
            PublishWritingBatch();
            PrepareNotification(notify, notifyContext);
        }
    }
    Notify(notify, notifyContext);
}

const FLogBatch* FWorkerLogChannel::TryBeginOutput() noexcept
{
    EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
    if (_Failed.load(std::memory_order_acquire) || _OutputInProgress || _Ready.empty())
        return nullptr;

    _OutputInProgress = true;
    return &_Ready.front().Batch;
}

bool FWorkerLogChannel::FinishOutput() noexcept
{
    // 처리한 묶음을 옮겨 둘 임시 목록을 만들 때 할당이 일어날 수 있다.
    try {
        std::list<FEntry> retired;
        uint64 releasedBytes{};
        {
            EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
            if (!_OutputInProgress || _Ready.empty())
                return false;
            const FEntry& entry{ _Ready.front() };
            releasedBytes = entry.ReservedBytes;
            // 소비자가 처리한 묶음을 분리하되, 해제가 끝날 때까지 예약량을 유지한다.
            retired.splice(retired.end(), _Ready, _Ready.begin());
        }
        // 레코드의 문자열과 메모리 풀 반환은 생산자와 공유하는 잠금 밖에서 수행한다.
        retired.clear();
        {
            EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
            _ReservedBytes -= releasedBytes;
            _OutputInProgress = false;
        }
        return true;
    }
    catch (...) { MarkFailure(std::current_exception()); return false; }
}

bool FWorkerLogChannel::AppendRecord(SHPTR<FLogRecord>& record, FNotify& notify, void*& context) noexcept
{
    // 성공 접수 전에 큐 노드와 묶음의 64개 포인터 공간을 마련한다.
    // 같은 할당자의 list::splice는 인계 시 새 노드를 할당하지 않는다.
    const bool newBatch{ _Writing.empty() };
    try {
        if (newBatch)
            _Writing.emplace_back();
        
        FEntry& entry{ _Writing.back() };
        if (newBatch) {
            entry.Batch.ChannelID = _ChannelID;
            entry.Batch.Records.reserve(LOG_MAX_BATCH_RECORDS);
        }
        const uint64 batchCharge{ newBatch
            ? static_cast<uint64>(sizeof(FEntry) + 2 * sizeof(void*) +
                entry.Batch.Records.capacity() * sizeof(SHPTR<FLogRecord>))
            : 0ULL };
        // 논리 예약량이며 풀 반올림과 할당자 내부 오버헤드 전체의 실측값은 아니다.
        const uint64 recordCharge{ static_cast<uint64>(sizeof(FLogRecord) +
            record->Message.capacity() + 1 + 2 * sizeof(void*)) };
        if (batchCharge > ChannelBudget || recordCharge > ChannelBudget - batchCharge ||
            _ReservedBytes > ChannelBudget - batchCharge - recordCharge) {
            if (newBatch) _Writing.clear();
            return false;
        }
        const bool firstRecord = entry.Batch.Records.empty();
        if (firstRecord)
            entry.FirstRecordTime = STEADY_CLOCK::now();
        entry.Batch.Records.push_back(std::move(record));
        entry.ReservedBytes += batchCharge + recordCharge;
        _ReservedBytes += batchCharge + recordCharge;
        ++_AcceptedCount;
        const bool batchReady = entry.Batch.Records.size() == LOG_MAX_BATCH_RECORDS;
        if (batchReady)
            PublishWritingBatch();
        if (firstRecord || batchReady)
            PrepareNotification(notify, context);
        return true;
    }
    catch (...) {
        if (newBatch) _Writing.clear();
        if (!_Failed.load(std::memory_order_acquire))
            _Failure = std::current_exception();
        _Failed.store(true, std::memory_order_release);
        PrepareNotification(notify, context);
        return false;
    }
}

void FWorkerLogChannel::PrepareNotification(FNotify& notify, void*& context) noexcept
{
    notify = _Notifier;
    context = _NotifyContext;
}

void FWorkerLogChannel::Notify(FNotify notify, void* context) noexcept
{
    if (notify && !notify(context))
        MarkFailure(nullptr);
}

void FWorkerLogChannel::MarkFailure(EXPECTION_PTR failure) noexcept
{
    FNotify notify{};
    void* context{};
    {
        EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
        if (_Failed.load(std::memory_order_acquire))
            return;
        _Failure = std::move(failure);
        _Failed.store(true, std::memory_order_release);
        notify = _Notifier;
        context = _NotifyContext;
    }
    if (notify)
        notify(context);
}

const FWorkerLogChannel::FEntry* FWorkerLogChannel::GetWritingEntry() const noexcept
{
    if (_Writing.empty() || _Writing.front().Batch.Records.empty())
        return nullptr;
    return &_Writing.front();
}

void FWorkerLogChannel::PublishWritingBatch() noexcept
{
    if (GetWritingEntry())
        _Ready.splice(_Ready.end(), _Writing, _Writing.begin());
}

SPACE_T
