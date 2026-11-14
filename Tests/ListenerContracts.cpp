#include "CoreDefine.h"
#include "UCoreInstance.h"
#include "UNetworkManager.h"
#include "UListener.h"
#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <vector>

using namespace Core;
using namespace std::chrono_literals;
static void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }

class UTestListener final : public UListener {
public:
    explicit UTestListener(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context,
        FUNC<void(TCPSOCKET)> handler)
        : UListener{ coreInstance, context }, _Handler{ std::move(handler) } {}

protected:
    void HandleAcceptedSocket(TCPSOCKET socket) override { _Handler(std::move(socket)); }

private:
    FUNC<void(TCPSOCKET)> _Handler;
};

static_assert(std::is_abstract_v<UListener>);
static_assert(!std::is_abstract_v<UTestListener>);

struct Fixture {
    SHPTR<UCoreInstance> CoreInstance;
    SHPTR<UNetworkManager> Manager;
    SHPTR<UListener> Listener;
    TCP_ENDPOINT Endpoint;
    SHARED_MUTEX Mutex;
    COND_VARIABLE Changed;
    std::vector<TCPSOCKET> Accepted;
    std::vector<std::jthread> Workers;
    int Failures{0};
    int Exited{0};
    bool BlockCallback{false};
    bool CallbackEntered{false};
    bool ReleaseCallback{false};

    Fixture() {
        CoreInstance = MakeShared<UCoreInstance>();
        Require(CoreInstance->Initialize(), "initialize");
        Manager = CoreInstance->GetNetworkManager();
        Listener = MakeShared<UTestListener>(CoreInstance, Manager->GetIOContext(),
            [this](TCPSOCKET socket) { AcceptedHandler(std::move(socket)); });
        // 시험용 포트는 리스너 API가 아닌 별도 수락기로 정한다.
        TCPACCEPTOR probe{Manager->GetIOContext(), TCP_ENDPOINT{ASIO::ip::address_v4::loopback(), 0}};
        Endpoint = probe.local_endpoint();
        probe.close();
    }
    void WorkersStart() {
        for (int i=0; i<2; ++i) Workers.emplace_back([this] {
            try { Manager->GetIOContext().run(); }
            catch (...) { EXCLUSIVE_LOCK_GUARD lock(Mutex); ++Failures; }
            { EXCLUSIVE_LOCK_GUARD lock(Mutex); ++Exited; }
            Changed.notify_all();
        });
    }
    void AcceptedHandler(TCPSOCKET socket) {
        std::unique_lock<SHARED_MUTEX> lock(Mutex);
        Accepted.push_back(std::move(socket));
        CallbackEntered = true;
        Changed.notify_all();
        if (BlockCallback) Changed.wait(lock, [this] { return ReleaseCallback; });
    }
    void ErrorHandler(const BOOST_ERROR_CODE&) {
        { EXCLUSIVE_LOCK_GUARD lock(Mutex); ++Failures; }
        Changed.notify_all();
    }
    bool Start(BOOST_ERROR_CODE& ec) {
        return Listener->Start(Endpoint,
            [this](const BOOST_ERROR_CODE& error) { ErrorHandler(error); }, ec);
    }
    bool WaitAccepted(size_t count) {
        std::unique_lock<SHARED_MUTEX> lock(Mutex);
        return Changed.wait_for(lock,3s,[&] { return Accepted.size() >= count || Failures; }) && Failures == 0 && Accepted.size() >= count;
    }
    bool Shutdown() {
        BOOST_ERROR_CODE error; Listener->Stop(error);
        Manager->ReleaseWork();
        std::unique_lock<SHARED_MUTEX> lock(Mutex);
        bool exited = Changed.wait_for(lock, 3s, [&] { return Exited == static_cast<int>(Workers.size()); });
        return !error && exited;
    }
    ~Fixture() {
        { EXCLUSIVE_LOCK_GUARD lock(Mutex); ReleaseCallback = true; }
        Changed.notify_all();
        BOOST_ERROR_CODE ignored;
        if (Listener) Listener->Stop(ignored);
        if (Manager) { Manager->ReleaseWork(); Manager->GetIOContext().stop(); }
        for (auto& worker : Workers) if (worker.joinable()) worker.join();
        Accepted.clear();
    }
};

static void TestAcceptAndShutdown() {
    Fixture f; BOOST_ERROR_CODE ec;
    Require(f.Start(ec) && !ec, "listener start");
    auto endpoint = f.Endpoint;
    auto conflict = MakeShared<UTestListener>(f.CoreInstance, f.Manager->GetIOContext(), [](TCPSOCKET){});
    BOOST_ERROR_CODE busy;
    Require(!conflict->Start(endpoint, [](const BOOST_ERROR_CODE&){}, busy) && bool(busy), "occupied endpoint");
    f.WorkersStart();
    IOCONTEXT clientContext;
    TCPSOCKET first{clientContext}, second{clientContext};
    first.connect(endpoint);
    Require(f.WaitAccepted(1), "first connection accepted");
    second.connect(endpoint);
    Require(f.WaitAccepted(2), "second connection accepted");
    char sent='X', received=0;
    ASIO::write(first, ASIO::buffer(&sent,1));
    // The first accepted connection corresponds to the first connect call.
    { EXCLUSIVE_LOCK_GUARD lock(f.Mutex); ASIO::read(f.Accepted[0],ASIO::buffer(&received,1)); }
    Require(received == sent, "transferred socket remains usable");
    Require(f.Shutdown(), "cancel pending accept and stop workers");
    Require(f.Failures == 0, "stop must not report accept error");
}

struct ReenterOnDestroy {
    WKPTR<UListener> Listener;
    std::atomic<bool>* Destroyed;
    ~ReenterOnDestroy() {
        if (auto listener=Listener.lock()) { BOOST_ERROR_CODE ignored; listener->Stop(ignored); }
        Destroyed->store(true);
    }
};

static void TestCallbackDestruction() {
    Fixture f; BOOST_ERROR_CODE ec; std::atomic<bool> destroyed{false};
    auto capture=std::make_shared<ReenterOnDestroy>();
    capture->Listener=f.Listener; capture->Destroyed=&destroyed;
    Require(f.Listener->Start(TCP_ENDPOINT{ASIO::ip::address_v4::loopback(),0},
        [capture](const BOOST_ERROR_CODE&){}, ec), "reentry start");
    capture.reset();
    f.Listener->Stop(ec);
    Require(!ec && destroyed.load(), "callback destructor can call listener Stop");
    f.Manager->ReleaseWork(); f.Manager->GetIOContext().run();
}

static void TestStopBeforeWorkers() {
    Fixture f; BOOST_ERROR_CODE ec;
    Require(f.Start(ec), "start before workers");
    f.Listener->Stop(ec); Require(!ec, "stop before workers");
    Require(!f.Start(ec) && ec == ASIO::error::already_started, "successful listener cannot restart");
    f.Manager->ReleaseWork(); f.Manager->GetIOContext().run();
    Require(f.Accepted.empty() && f.Failures == 0, "cancel completion before workers");
}

static void TestCallbackException() {
    Fixture f; BOOST_ERROR_CODE ec;
    f.Listener = MakeShared<UTestListener>(f.CoreInstance, f.Manager->GetIOContext(),
        [](TCPSOCKET) { throw std::runtime_error("callback failure"); });
    Require(f.Listener->Start(f.Endpoint,
        [&f](const BOOST_ERROR_CODE& error) { f.ErrorHandler(error); }, ec), "exception start");
    auto endpoint=f.Endpoint;
    f.WorkersStart();
    IOCONTEXT clientContext; TCPSOCKET client{clientContext}; client.connect(endpoint);
    { std::unique_lock<SHARED_MUTEX> lock(f.Mutex);
      Require(f.Changed.wait_for(lock,3s,[&]{return f.Failures != 0;}), "callback exception reaches worker"); }
    TCPSOCKET afterFailure{clientContext};
    afterFailure.connect(endpoint, ec); Require(bool(ec), "listener closes after callback exception");
    Require(f.Shutdown(), "exception shutdown");
}

static void TestStopDuringCallback() {
    Fixture f; BOOST_ERROR_CODE ec;
    f.BlockCallback=true;
    Require(f.Start(ec), "start race");
    auto endpoint=f.Endpoint;
    f.WorkersStart();
    IOCONTEXT clientContext; TCPSOCKET client{clientContext}; client.connect(endpoint);
    Require(f.WaitAccepted(1), "blocked callback started");
    f.Listener->Stop(ec); Require(!ec, "stop during callback");
    TCPSOCKET afterStop{clientContext};
    afterStop.connect(endpoint, ec);
    Require(bool(ec), "listener must not accept after stop");
    { EXCLUSIVE_LOCK_GUARD lock(f.Mutex); f.ReleaseCallback=true; }
    f.Changed.notify_all();
    Require(f.Shutdown(), "shutdown after callback");
}

static void TestListenerLifetimeDuringCallback() {
    Fixture f; BOOST_ERROR_CODE ec;
    f.BlockCallback=true;
    Require(f.Start(ec), "lifetime start");
    WKPTR<UListener> weak=f.Listener;
    f.WorkersStart();
    IOCONTEXT clientContext; TCPSOCKET client{clientContext}; client.connect(f.Endpoint);
    Require(f.WaitAccepted(1), "lifetime callback started");
    f.Listener->Stop(ec); Require(!ec, "lifetime stop");
    f.Listener.reset();
    Require(!weak.expired(), "listener must live during its callback");
    { EXCLUSIVE_LOCK_GUARD lock(f.Mutex); f.ReleaseCallback=true; }
    f.Changed.notify_all();
    f.Manager->ReleaseWork();
    { std::unique_lock<SHARED_MUTEX> lock(f.Mutex);
      Require(f.Changed.wait_for(lock,3s,[&]{return f.Exited == static_cast<int>(f.Workers.size());}), "lifetime workers exit"); }
    Require(weak.expired(), "listener released after callback finishes");
}

static void TestAcceptStopRace() {
    for (int i=0; i<25; ++i) {
        Fixture f; BOOST_ERROR_CODE ec;
        Require(f.Start(ec), "race start");
        f.WorkersStart();
        IOCONTEXT clientContext;
        TCPSOCKET client{clientContext};
        client.connect(f.Endpoint, ec);
        Require(!ec, "race client connect");
        f.Listener->Stop(ec);
        Require(!ec, "race stop");
        Require(f.Shutdown(), "race worker shutdown");
        Require(f.Failures == 0, "stop must not report an accept failure");
    }
}

int main() {
    int result=0;
    try {
        TestAcceptAndShutdown(); std::cout << "PASS accepted sockets, occupied port and shutdown\n";
        TestStopBeforeWorkers(); std::cout << "PASS cancellation before workers\n";
        TestCallbackDestruction(); std::cout << "PASS callback destruction reentry\n";
        TestStopDuringCallback(); std::cout << "PASS stop while callback active\n";
        TestAcceptStopRace(); std::cout << "PASS accept and stop race\n";
        TestListenerLifetimeDuringCallback(); std::cout << "PASS listener lifetime during callback\n";
        TestCallbackException(); std::cout << "PASS callback exception stops listener and reaches worker\n";
    }
    catch (const std::exception& error) { std::cerr << "FAIL " << error.what() << '\n'; result=1; }
    GThreadMemoryCache.FlushToCentral();
    UCoreGlobal::DeleteInstance();
    return result;
}
