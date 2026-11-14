#include "UJobScheduler.h"
#include <atomic>
#include <chrono>
#include <future>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

static bool Check(bool cond, const char* message)
{
    if (!cond) std::cerr << "FAIL: " << message << '\n';
    return cond;
}

static bool TestManager()
{
    Core::UThreadManager manager;
    Core::UThreadManager::FThreadToken token{};
    std::atomic<int> ran{};
    if (!Check(manager.Launch([&] { ++ran; }, token), "launch")) return false;
    if (!Check(token != 0 && manager.Join(token), "individual join")) return false;
    if (!Check(ran == 1 && !manager.Join(token), "join only once")) return false;
    if (!Check(manager.Launch([] { throw std::runtime_error("expected"); }, token), "exception launch")) return false;
    std::exception_ptr failure;
    if (!Check(manager.Join(token, &failure) && failure != nullptr, "exception returned")) return false;
    if (!Check(manager.UnhandledExceptionCount() == 1, "exception count")) return false;
    if (!Check(manager.Launch([&] { if (manager.JoinAll()) ++ran; }, token), "self join launch")) return false;
    if (!Check(manager.Join(token) && ran == 1, "reject join from managed thread")) return false;
    if (!Check(manager.JoinAll(), "join all")) return false;
    return Check(!manager.Launch([] {}, token) && token == 0, "launch after close");
}

static bool TestConcurrentJobs()
{
    Core::UThreadManager manager;
    Core::UJobScheduler scheduler(manager, 64);
    if (!Check(!scheduler.Start(0), "reject zero workers")) return false;
    if (!Check(scheduler.Start(4), "start workers")) return false;
    if (!Check(!scheduler.Start(4), "reject restart")) return false;
    constexpr int producers = 4, perProducer = 300;
    std::vector<std::atomic<int>> seen(producers * perProducer);
    std::atomic<bool> failed{};
    std::vector<std::thread> threads;
    for (int p = 0; p < producers; ++p) threads.emplace_back([&, p] {
        for (int i = 0; i < perProducer; ++i) {
            const int index = p * perProducer + i;
            for (;;) {
                auto result = scheduler.TrySubmit([&, index] { ++seen[index]; });
                if (result == Core::UJobScheduler::ESubmitResult::Accepted) break;
                if (result != Core::UJobScheduler::ESubmitResult::QueueFull) { failed = true; return; }
                std::this_thread::yield();
            }
        }
    });
    for (auto& t : threads) t.join();
    if (!Check(!failed && scheduler.Shutdown(), "drain all accepted work")) return false;
    for (auto& n : seen) if (!Check(n.load() == 1, "each accepted job once")) return false;
    return Check(scheduler.TrySubmit([] {}) == Core::UJobScheduler::ESubmitResult::NotRunning,
        "submit after shutdown");
}

static bool TestFullAndDrain()
{
    Core::UThreadManager manager;
    Core::UJobScheduler scheduler(manager, 2);
    if (!Check(scheduler.Start(1), "start gated worker")) return false;
    std::promise<void> started, release;
    auto startedFuture = started.get_future();
    auto releaseFuture = release.get_future().share();
    auto submitted = scheduler.TrySubmit([&] { started.set_value(); releaseFuture.wait(); });
    if (!Check(submitted == Core::UJobScheduler::ESubmitResult::Accepted, "submit blocker")) return false;
    if (!Check(startedFuture.wait_for(5s) == std::future_status::ready, "worker started")) { release.set_value(); return false; }
    std::atomic<int> completed{};
    auto one = scheduler.TrySubmit([&] { ++completed; });
    auto two = scheduler.TrySubmit([&] { ++completed; });
    auto full = scheduler.TrySubmit([&] { ++completed; });
    auto stop = std::async(std::launch::async, [&] { return scheduler.Shutdown(); });
    bool waits = stop.wait_for(50ms) == std::future_status::timeout;
    release.set_value();
    bool stopped = stop.get();
    bool okay = one == Core::UJobScheduler::ESubmitResult::Accepted &&
        two == Core::UJobScheduler::ESubmitResult::Accepted &&
        full == Core::UJobScheduler::ESubmitResult::QueueFull;
    return Check(okay && waits && stopped && completed == 2, "queue bound and drain");
}

static bool TestJobException()
{
    Core::UThreadManager manager;
    Core::UJobScheduler scheduler(manager, 8);
    if (!Check(scheduler.Start(1), "start exception worker")) return false;
    std::atomic<int> after{};
    if (!Check(scheduler.TrySubmit([] { throw std::runtime_error("expected"); }) ==
        Core::UJobScheduler::ESubmitResult::Accepted, "exception job accepted")) return false;
    if (!Check(scheduler.TrySubmit([&] { ++after; }) ==
        Core::UJobScheduler::ESubmitResult::Accepted, "next job accepted")) return false;
    return Check(scheduler.Shutdown() && scheduler.FailedJobCount() == 1 && after == 1,
        "worker survives job exception");
}

static bool TestSubmitShutdownRace()
{
    Core::UThreadManager manager;
    Core::UJobScheduler scheduler(manager, 64);
    if (!Check(scheduler.Start(4), "start race workers")) return false;
    std::atomic<int> ready{}, accepted{}, executed{};
    std::atomic<bool> go{}, unexpected{};
    std::vector<std::thread> producers;
    for (int p = 0; p < 4; ++p) producers.emplace_back([&] {
        ++ready;
        while (!go.load()) std::this_thread::yield();
        for (int i = 0; i < 10000; ++i) {
            auto result = scheduler.TrySubmit([&] { ++executed; });
            if (result == Core::UJobScheduler::ESubmitResult::Accepted) ++accepted;
            else if (result == Core::UJobScheduler::ESubmitResult::NotRunning) return;
            else if (result != Core::UJobScheduler::ESubmitResult::QueueFull) { unexpected = true; return; }
        }
    });
    while (ready.load() != 4) std::this_thread::yield();
    go = true;
    while (accepted.load() == 0 && !unexpected.load()) std::this_thread::yield();
    const bool stopped = scheduler.Shutdown();
    for (auto& t : producers) t.join();
    return Check(stopped && !unexpected && accepted > 0 && accepted == executed,
        "submit/shutdown race drains each accepted job");
}

static bool TestShutdownFromWorker()
{
    Core::UThreadManager manager;
    Core::UJobScheduler scheduler(manager, 4);
    if (!Check(scheduler.Start(1), "start self shutdown worker")) return false;
    std::promise<bool> result;
    auto future = result.get_future();
    if (!Check(scheduler.TrySubmit([&] { result.set_value(scheduler.Shutdown()); }) ==
        Core::UJobScheduler::ESubmitResult::Accepted, "submit self shutdown")) return false;
    if (!Check(future.wait_for(5s) == std::future_status::ready, "self shutdown returned")) return false;
    return Check(!future.get() && scheduler.Shutdown(), "self shutdown rejected without deadlock");
}

int main()
{
    bool ok = TestManager() && TestConcurrentJobs() && TestFullAndDrain() && TestJobException() &&
        TestSubmitShutdownRace() && TestShutdownFromWorker();
    Core::UCoreGlobal::DeleteInstance();
    if (ok) std::cout << "ThreadSchedulerTests: PASS\n";
    return ok ? 0 : 1;
}
