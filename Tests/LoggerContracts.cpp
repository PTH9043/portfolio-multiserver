#include "CoreDefine.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include "FWorkerLogChannel.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace Core;
using namespace std::chrono_literals;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

// The worker is always stopped and joined before the captured state is destroyed.
struct LoggerFixture
{
    SHPTR<UCoreInstance> CoreInstance;
    SHPTR<ULogger> Logger;
    SHARED_MUTEX Mutex;
    COND_VARIABLE Changed;
    std::multiset<std::string> SharedMessages;
    std::multiset<std::string> WorkerMessages;
    std::atomic<int> Failures{0};
    std::thread Consumer;

    LoggerFixture()
    {
        CoreInstance = MakeShared<UCoreInstance>();
        Require(CoreInstance->Initialize(), "core initialization");
        Logger = CoreInstance->GetLogger();
    }
    void Start()
    {
        Require(Logger->SetWorkerBatchHandler([this](const FLogBatch& batch) {
            EXCLUSIVE_LOCK_GUARD lock(Mutex);
            for (const auto& record : batch.Records) WorkerMessages.emplace(record->Message.View());
            Changed.notify_all();
        }), "worker batch handler");
        Require(Logger->Start([this](const LOGBATCH_CONTAINER& batch) {
            EXCLUSIVE_LOCK_GUARD lock(Mutex);
            for (const auto& record : batch) SharedMessages.emplace(record->Message.View());
            Changed.notify_all();
        }, [this](EXPECTION_PTR) { ++Failures; Changed.notify_all(); }), "logger start");
        Consumer = std::thread([this] { ULogger::RunWorker(Logger.get()); });
    }
    bool WaitFor(size_t shared, size_t worker)
    {
        std::unique_lock<SHARED_MUTEX> lock(Mutex);
        return Changed.wait_for(lock, 3s, [&] {
            return (SharedMessages.size() >= shared && WorkerMessages.size() >= worker) || Failures.load() != 0;
        }) && Failures.load() == 0 && SharedMessages.size() >= shared && WorkerMessages.size() >= worker;
    }
    void Stop()
    {
        if (Consumer.joinable()) {
            Logger->RequestStop();
            Consumer.join();
        }
    }
    ~LoggerFixture() { Stop(); }
};

static void TestChannelDeadline()
{
    Require(LOG_MAX_BATCH_WAIT == 100ms && LOG_MAX_BATCH_RECORDS == 64, "batch contract constants");
    FWorkerLogChannel channel{7};
    Require(channel.TryLog(ELogLevel::Info, "deadline"), "channel record");
    auto state = channel.GetNotificationState();
    Require(state.Valid && state.HasWritingBatch, "channel writing state");
    Require(!channel.PublishExpiredBatch(state.FirstRecordTime + LOG_MAX_BATCH_WAIT - 1ms), "early publish");
    Require(channel.PublishExpiredBatch(state.FirstRecordTime + LOG_MAX_BATCH_WAIT), "deadline publish");
    auto* batch = channel.TryBeginOutput();
    Require(batch && batch->Records.size() == 1 && batch->Records[0]->Message.View() == "deadline", "deadline batch");
    Require(channel.FinishOutput(), "deadline output completion");
    FWorkerLogChannel full{8};
    for (int i = 0; i < 63; ++i) Require(full.TryLog(ELogLevel::Info, "count"), "count log");
    Require(full.TryBeginOutput() == nullptr, "63 records must remain in writing batch");
    Require(full.TryLog(ELogLevel::Info, "count"), "64th record");
    auto* fullBatch = full.TryBeginOutput();
    Require(fullBatch && fullBatch->Records.size() == 64, "64 records must publish");
    Require(full.FinishOutput(), "count output completion");
}

static void TestTimerAndCount()
{
    LoggerFixture fixture;
    auto channel = fixture.Logger->RegisterWorkerChannel(1);
    Require(bool(channel), "channel registration");
    fixture.Start();
    Require(fixture.Logger->TryLog(ELogLevel::Info, "shared-timer"), "shared timer log");
    Require(channel->TryLog(ELogLevel::Info, "worker-timer"), "worker timer log");
    // Both partial batches must arrive while the logger is running, without shutdown flushing them.
    Require(fixture.WaitFor(1, 1), "partial batch timer");
    for (int i = 0; i < 64; ++i) {
        Require(fixture.Logger->TryLog(ELogLevel::Info, "shared-" + std::to_string(i)), "shared full batch");
        Require(channel->TryLog(ELogLevel::Info, "worker-" + std::to_string(i)), "worker full batch");
    }
    Require(fixture.WaitFor(65, 65), "full batch output");
    fixture.Stop();
    Require(fixture.Failures == 0, "logger failure");
    Require(fixture.SharedMessages.size() == 65 && fixture.WorkerMessages.size() == 65, "batch count");
}

static void TestConcurrentShutdown()
{
    for (int round = 0; round < 30; ++round) {
        LoggerFixture fixture;
        auto first = fixture.Logger->RegisterWorkerChannel(10);
        auto second = fixture.Logger->RegisterWorkerChannel(11);
        Require(bool(first) && bool(second), "concurrent channels");
        fixture.Start();
        std::atomic<int> rejected{0};
        auto produce = [&](int id, SHPTR<FWorkerLogChannel> channel) {
            for (int i = 0; i < 80; ++i) {
                auto suffix = std::to_string(id) + "-" + std::to_string(i);
                if (!fixture.Logger->TryLog(ELogLevel::Info, "S" + suffix)) ++rejected;
                if (!channel->TryLog(ELogLevel::Info, "W" + suffix)) ++rejected;
            }
        };
        std::jthread a(produce, 0, first);
        std::jthread b(produce, 1, second);
        a.join(); b.join();
        // RequestStop's documented contract requires every producer to have finished.
        fixture.Stop();
        Require(rejected == 0 && fixture.Failures == 0, "concurrent log rejected or logger failure");
        std::multiset<std::string> expectedShared, expectedWorker;
        for (int id = 0; id < 2; ++id) for (int i = 0; i < 80; ++i) {
            auto suffix = std::to_string(id) + "-" + std::to_string(i);
            expectedShared.emplace("S" + suffix);
            expectedWorker.emplace("W" + suffix);
        }
        Require(fixture.SharedMessages == expectedShared, "shared missing or duplicate message");
        Require(fixture.WorkerMessages == expectedWorker, "worker missing or duplicate message");
    }
}

int main()
{
    int result = 0;
    try {
        TestChannelDeadline(); std::cout << "PASS channel 100ms and 64-record boundaries\n";
        TestTimerAndCount(); std::cout << "PASS partial and full batch while running\n";
        TestConcurrentShutdown(); std::cout << "PASS concurrent producers and final drain, 30 rounds\n";
    }
    catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n'; result = 1;
    }
    GThreadMemoryCache.FlushToCentral();
    UCoreGlobal::DeleteInstance();
    return result;
}
