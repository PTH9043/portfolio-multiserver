#include "CoreDefine.h"
#include "UCoreInstance.h"
#include "UListener.h"
#include "USession.h"
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace Core;
using namespace std::chrono_literals;
static_assert(PACKET_BUFFER_SIZE == 1024, "session packet buffer must be 1024 bytes");

static void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}

struct FTestState {
    SHARED_MUTEX Mutex;
    COND_VARIABLE Changed;
    std::vector<std::pair<uint16, std::vector<uint8>>> Packets;
    std::vector<ESessionSendAdmission> Admissions;
    std::vector<std::pair<BOOST_ERROR_CODE, uint64>> Completions;
    BOOST_ERROR_CODE CloseReason;
    int Closed{0};
    int Exceptions{0};
    bool ThrowPacket{false};
    bool ThrowClose{false};
};

class UTestSession final : public USession {
public:
    UTestSession(SHPTR<UCoreInstance> core, TCPSOCKET socket, FTestState& state)
        : USession{std::move(core), std::move(socket),
            [&state](const BOOST_ERROR_CODE& ec) {
                bool shouldThrow;
                { EXCLUSIVE_LOCK_GUARD lock(state.Mutex); state.CloseReason = ec; ++state.Closed; shouldThrow = state.ThrowClose; }
                state.Changed.notify_all();
                if (shouldThrow) throw std::runtime_error("close callback failure");
            },
            [&state](EXPECTION_PTR error) {
                { EXCLUSIVE_LOCK_GUARD lock(state.Mutex); if (error) ++state.Exceptions; }
                state.Changed.notify_all();
            }}, _Test{state} {}
protected:
    void HandlePacket(const FPacketHeader& header, const uint8* body, uint64 size) override {
        EXCLUSIVE_LOCK_GUARD lock(_Test.Mutex);
        if (_Test.ThrowPacket) throw std::runtime_error("packet handler failure");
        _Test.Packets.emplace_back(header.type, std::vector<uint8>(body, body + size));
        _Test.Changed.notify_all();
    }
private:
    FTestState& _Test;
};

struct Fixture {
    FTestState State;
    IOCONTEXT ServerContext;
    ASIO::executor_work_guard<IOCONTEXT::executor_type> Guard{ASIO::make_work_guard(ServerContext)};
    IOCONTEXT ClientContext;
    TCPSOCKET Client{ClientContext};
    SHPTR<UCoreInstance> CoreInstance{MakeShared<UCoreInstance>()};
    SHPTR<UTestSession> Session;
    std::vector<std::thread> Workers;

    explicit Fixture(bool smallBuffers = false) {
        TCPACCEPTOR acceptor{ServerContext, TCP_ENDPOINT{ASIO::ip::address_v4::loopback(), 0}};
        if (smallBuffers) {
            Client.open(ASIO::ip::tcp::v4());
            Client.set_option(ASIO::socket_base::receive_buffer_size(4096));
        }
        Client.connect(acceptor.local_endpoint());
        TCPSOCKET socket{ServerContext};
        acceptor.accept(socket);
        if (smallBuffers) socket.set_option(ASIO::socket_base::send_buffer_size(4096));
        Session = MakeShared<UTestSession>(CoreInstance, std::move(socket), State);
    }
    void Start() {
        Require(Session->Start(), "post session start");
        for (int i = 0; i < 2; ++i) Workers.emplace_back([this] {
            try { ServerContext.run(); }
            catch (...) {
                { EXCLUSIVE_LOCK_GUARD lock(State.Mutex); ++State.Exceptions; }
                State.Changed.notify_all();
            }
            GThreadMemoryCache.FlushToCentral();
        });
    }
    template<class P> void Wait(P pred, const char* message) {
        std::unique_lock<SHARED_MUTEX> lock(State.Mutex);
        Require(State.Changed.wait_for(lock, 5s, pred), message);
    }
    void WaitClosed() { Wait([this] { return State.Closed != 0; }, "session close timeout"); }
    bool Send(uint16 type, BUFFER body) {
        return Session->RequestSend(type, std::move(body),
            [this](ESessionSendAdmission value) {
                { EXCLUSIVE_LOCK_GUARD lock(State.Mutex); State.Admissions.push_back(value); }
                State.Changed.notify_all();
            },
            [this](const BOOST_ERROR_CODE& ec, uint64 bytes) {
                { EXCLUSIVE_LOCK_GUARD lock(State.Mutex); State.Completions.emplace_back(ec, bytes); }
                State.Changed.notify_all();
            });
    }
    ~Fixture() {
        if (Session) Session->RequestStop();
        Guard.reset();
        ServerContext.stop();
        for (auto& worker : Workers) if (worker.joinable()) worker.join();
        Session.reset();
    }
};

static std::vector<uint8> Frame(uint16 type, const std::vector<uint8>& body) {
    FPacketHeader header{type, static_cast<uint16>(body.size())};
    std::vector<uint8> data(sizeof(header) + body.size());
    std::memcpy(data.data(), &header, sizeof(header));
    if (!body.empty()) std::memcpy(data.data() + sizeof(header), body.data(), body.size());
    return data;
}

static void Write(TCPSOCKET& socket, const std::vector<uint8>& bytes) {
    ASIO::write(socket, ASIO::buffer(bytes));
}

static std::vector<uint8> ReadExact(TCPSOCKET& socket, size_t size) {
    socket.non_blocking(true);
    std::vector<uint8> result(size);
    size_t received = 0;
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (received < size && std::chrono::steady_clock::now() < deadline) {
        BOOST_ERROR_CODE ec;
        received += socket.read_some(ASIO::buffer(result.data() + received, size - received), ec);
        if (ec == ASIO::error::would_block || ec == ASIO::error::try_again) {
            std::this_thread::sleep_for(1ms); continue;
        }
        if (ec) throw std::runtime_error("client read failed after " + std::to_string(received) + " bytes: " + std::to_string(ec.value()));
    }
    Require(received == size, "client read timeout");
    return result;
}

static void TestReceive() {
    Fixture f; f.Start();
    auto a = Frame(1, {1,2,3,4,5,6,7,8});
    auto b = Frame(2, {9,10,11,12,13,14,15,16});
    Write(f.Client, {a.begin(), a.begin() + 8});
    std::vector<uint8> middle(a.begin() + 8, a.end());
    middle.insert(middle.end(), b.begin(), b.begin() + 4);
    Write(f.Client, middle);
    f.Wait([&] { return f.State.Packets.size() == 1; }, "first fragmented frame");
    Write(f.Client, {b.begin() + 4, b.end()});
    auto empty = Frame(99, {}), c = Frame(3, {42});
    empty.insert(empty.end(), c.begin(), c.end()); Write(f.Client, empty);
    auto maximum = Frame(4, std::vector<uint8>(1020, 77)); Write(f.Client, maximum);
    f.Wait([&] { return f.State.Packets.size() == 4; }, "all valid frames");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Packets[0].second == std::vector<uint8>({1,2,3,4,5,6,7,8}), "A body");
        Require(f.State.Packets[1].second == std::vector<uint8>({9,10,11,12,13,14,15,16}), "B body after compaction");
        Require(f.State.Packets[2].first == 3 && f.State.Packets[3].second == std::vector<uint8>(1020, 77), "empty skip and max frame body");
    }
    FPacketHeader invalid{5, 1021};
    std::vector<uint8> bad(sizeof(invalid)); std::memcpy(bad.data(), &invalid, sizeof(invalid));
    auto after = Frame(6, {8}); bad.insert(bad.end(), after.begin(), after.end());
    Write(f.Client, bad); f.WaitClosed();
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(f.State.CloseReason == ASIO::error::message_size, "invalid length close reason");
    Require(f.State.Packets.size() == 4 && f.State.Closed == 1, "invalid frame stops parsing and closes once");
}

static void TestFragmentedStream() {
    std::vector<std::pair<uint16, std::vector<uint8>>> expected;
    std::vector<uint8> stream;
    for (uint16 i = 0; i < 160; ++i) {
        const size_t length = i % 7 == 0 ? 1020 : (i * 37) % 181;
        std::vector<uint8> body(length);
        for (size_t j = 0; j < length; ++j) body[j] = static_cast<uint8>((i + j * 31) & 255);
        auto bytes = Frame(i, body);
        stream.insert(stream.end(), bytes.begin(), bytes.end());
        if (!body.empty()) expected.emplace_back(i, std::move(body));
    }
    Fixture f; f.Start();
    uint32 sequence = 17;
    size_t offset = 0;
    while (offset < stream.size()) {
        sequence = sequence * 1664525u + 1013904223u;
        const size_t chunk = (std::min)(size_t(1 + sequence % 137), stream.size() - offset);
        ASIO::write(f.Client, ASIO::buffer(stream.data() + offset, chunk));
        offset += chunk;
    }
    BOOST_ERROR_CODE error; f.Client.shutdown(TCPSOCKET::shutdown_send, error);
    Require(!error, "fragmented stream EOF");
    f.WaitClosed();
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(!f.State.CloseReason && f.State.Packets == expected, "fragmented stream content and order");
}

static void TestGracefulSend() {
    Fixture f; f.Start();
    BUFFER first{0,255}, second{3}, rejected{4};
    Require(f.Send(11, first) && f.Send(12, second) && f.Send(14, BUFFER(1020, 8)) && f.Send(15, BUFFER{}), "post sends including maximum and empty frames");
    first[0] = 99;
    Require(f.Session->RequestStop(), "post normal stop");
    Require(f.Send(13, rejected), "post send after stop");
    f.Wait([&] { return f.State.Closed && f.State.Admissions.size() == 5 && f.State.Completions.size() == 4; }, "graceful callbacks");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        if (f.State.Completions[0].first || f.State.Completions[1].first || f.State.Completions[2].first || f.State.Completions[3].first || f.State.CloseReason)
            throw std::runtime_error("send errors: " + std::to_string(f.State.Completions[0].first.value()) + "," +
                std::to_string(f.State.Completions[1].first.value()) + ", close " + std::to_string(f.State.CloseReason.value()));
    }
    auto expected = Frame(11, {0,255}), next = Frame(12, {3});
    expected.insert(expected.end(), next.begin(), next.end());
    auto maximum = Frame(14, std::vector<uint8>(1020, 8));
    expected.insert(expected.end(), maximum.begin(), maximum.end());
    auto empty = Frame(15, {});
    expected.insert(expected.end(), empty.begin(), empty.end());
    Require(ReadExact(f.Client, expected.size()) == expected, "FIFO and owned send buffers");
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(f.State.Admissions[0] == ESessionSendAdmission::Accepted && f.State.Admissions[1] == ESessionSendAdmission::Accepted && f.State.Admissions[2] == ESessionSendAdmission::Accepted && f.State.Admissions[3] == ESessionSendAdmission::Accepted && f.State.Admissions[4] == ESessionSendAdmission::Closing, "send admissions");
    Require(!f.State.Completions[0].first && f.State.Completions[0].second == 6 && !f.State.Completions[1].first && f.State.Completions[1].second == 5 && !f.State.Completions[2].first && f.State.Completions[2].second == 1024 && !f.State.Completions[3].first && f.State.Completions[3].second == 4, "send completion bytes");
    Require(!f.State.CloseReason && f.State.Closed == 1, "normal close once");
}

static void TestRepeatedStartAndCallbackStop() {
    bool stopPosted = false;
    Fixture f;
    Require(f.Session->Start() && f.Session->Start(), "post repeated starts");
    f.Start();
    Write(f.Client, Frame(25, {1,2,3}));
    f.Wait([&] { return f.State.Packets.size() == 1; }, "receive after repeated starts");

    Require(f.Session->RequestSend(26, BUFFER{4,5},
        [&f, &stopPosted](ESessionSendAdmission value) {
            const bool posted = f.Session->RequestStop();
            { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); stopPosted = posted; f.State.Admissions.push_back(value); }
            f.State.Changed.notify_all();
        },
        [&f](const BOOST_ERROR_CODE& error, uint64 bytes) {
            { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); f.State.Completions.emplace_back(error, bytes); }
            f.State.Changed.notify_all();
        }), "post send with stop requested by admission callback");
    f.WaitClosed();
    Require(ReadExact(f.Client, 6) == Frame(26, {4,5}), "callback stop drains accepted frame");
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(stopPosted && f.State.Admissions.size() == 1 &&
        f.State.Admissions[0] == ESessionSendAdmission::Accepted, "callback stop posted after admission");
    Require(f.State.Packets.size() == 1 && f.State.Packets[0].second == std::vector<uint8>({1,2,3}),
        "repeated start processes one packet once");
    Require(f.State.Completions.size() == 1 && !f.State.Completions[0].first &&
        f.State.Completions[0].second == 6 && f.State.Closed == 1 && !f.State.CloseReason,
        "callback stop completes send before close");
}

static void TestCompletionCallbackRequestsStop() {
    bool stopPosted = false, latePosted = false;
    int lateCompletions = 0;
    Fixture f; f.Start();
    Require(f.Session->RequestSend(27, BUFFER{7},
        [&f](ESessionSendAdmission value) {
            { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); f.State.Admissions.push_back(value); }
            f.State.Changed.notify_all();
        },
        [&f, &stopPosted, &latePosted, &lateCompletions](const BOOST_ERROR_CODE& error, uint64 bytes) {
            const bool stopped = f.Session->RequestStop();
            const bool posted = f.Session->RequestSend(28, BUFFER{8},
                [&f](ESessionSendAdmission value) {
                    { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); f.State.Admissions.push_back(value); }
                    f.State.Changed.notify_all();
                },
                [&f, &lateCompletions](const BOOST_ERROR_CODE&, uint64) {
                    { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); ++lateCompletions; }
                    f.State.Changed.notify_all();
                });
            { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); stopPosted = stopped; latePosted = posted; f.State.Completions.emplace_back(error, bytes); }
            f.State.Changed.notify_all();
        }), "post send whose completion requests stop");
    f.Wait([&] { return f.State.Closed == 1 && f.State.Admissions.size() == 2; },
        "completion callback stop and late admission");
    Require(ReadExact(f.Client, 5) == Frame(27, {7}), "completion callback sends first frame");
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(stopPosted && latePosted && f.State.Admissions[0] == ESessionSendAdmission::Accepted &&
        f.State.Admissions[1] == ESessionSendAdmission::Closing, "completion callback post order");
    Require(f.State.Completions.size() == 1 && !f.State.Completions[0].first &&
        f.State.Completions[0].second == 5 && lateCompletions == 0 && !f.State.CloseReason,
        "completion callback closes once without completing rejected send");
}

static void TestStopBeforeStart() {
    Fixture f;
    Require(f.Session->RequestStop(), "post stop before start");
    Require(f.Send(29, BUFFER{9}), "post send after stop before start");
    f.Start();
    f.Wait([&] { return f.State.Closed == 1 && f.State.Admissions.size() == 1; },
        "stop before start completion");
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(!f.State.CloseReason && f.State.Admissions[0] == ESessionSendAdmission::Closing &&
        f.State.Completions.empty() && f.State.Packets.empty() && f.State.Exceptions == 0,
        "stop before start closes once and rejects sends");
}

static void TestEOFAndException() {
    {
        Fixture f; f.Start();
        Require(f.Send(7, BUFFER{7,8}), "send before EOF");
        f.Wait([&] { return !f.State.Admissions.empty(); }, "EOF send accepted");
        BOOST_ERROR_CODE ec; f.Client.shutdown(TCPSOCKET::shutdown_send, ec); Require(!ec, "client half close");
        f.WaitClosed();
        Require(ReadExact(f.Client, 6) == Frame(7, {7,8}), "send after receive EOF");
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(!f.State.CloseReason && f.State.Completions.size() == 1 && !f.State.Completions[0].first, "EOF drains accepted send");
    }
    {
        Fixture f; f.State.ThrowPacket = true; f.Start(); Write(f.Client, Frame(1, {1})); f.WaitClosed();
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Exceptions == 1 && f.State.CloseReason == ASIO::error::fault && f.State.Closed == 1, "packet exception reported and session closed");
    }
}

static void TestEOFWithPartialPacket() {
    Fixture f; f.Start();
    auto bytes = Frame(20, {1,2,3});
    auto incomplete = Frame(21, {4,5,6});
    bytes.insert(bytes.end(), incomplete.begin(), incomplete.begin() + 5);
    Write(f.Client, bytes);
    BOOST_ERROR_CODE error;
    f.Client.shutdown(TCPSOCKET::shutdown_send, error);
    Require(!error, "client EOF after partial packet");
    f.WaitClosed();
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(!f.State.CloseReason && f.State.Closed == 1, "EOF closes normally");
    Require(f.State.Packets.size() == 1 && f.State.Packets[0].first == 20 &&
        f.State.Packets[0].second == std::vector<uint8>({1,2,3}), "EOF processes complete packet only");
}

static void TestReceiveReset() {
    Fixture f; f.Start();
    f.Client.set_option(ASIO::socket_base::linger(true, 0));
    BOOST_ERROR_CODE error;
    f.Client.close(error);
    Require(!error, "client reset close");
    f.WaitClosed();
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(f.State.CloseReason == ASIO::error::connection_reset && f.State.Closed == 1,
        "receive error preserves reset reason");
}

static void TestCallbackExceptions() {
    Fixture f; f.Start();
    Require(f.Session->RequestSend(30, BUFFER{9},
        [](ESessionSendAdmission) { throw std::runtime_error("admission callback failure"); },
        [](const BOOST_ERROR_CODE&, uint64) { throw std::runtime_error("completion callback failure"); }), "post callbacks that throw");
    f.Wait([&] { return f.State.Exceptions == 2; }, "callback exception reporting");
    { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); f.State.ThrowClose = true; }
    Require(f.Session->RequestStop(), "close after callback exceptions");
    f.Wait([&] { return f.State.Closed == 1 && f.State.Exceptions == 3; }, "close callback exception reporting");
    Require(ReadExact(f.Client, 5) == Frame(30, {9}), "send continues after callback exception");
}

static void TestConcurrentAdmissions() {
    Fixture f; f.Start();
    std::vector<std::thread> senders;
    for (int n = 0; n < 4; ++n) senders.emplace_back([&f] {
        for (int i = 0; i < 30; ++i) Require(f.Send(8, BUFFER{8}), "concurrent post");
        GThreadMemoryCache.FlushToCentral();
    });
    Require(f.Session->RequestStop(), "concurrent stop post");
    for (auto& sender : senders) sender.join();
    f.Wait([&] { return f.State.Admissions.size() == 120 && f.State.Closed; }, "concurrent admission results");
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    size_t accepted = 0;
    for (auto a : f.State.Admissions) {
        Require(a == ESessionSendAdmission::Accepted || a == ESessionSendAdmission::Closing, "unexpected admission");
        if (a == ESessionSendAdmission::Accepted) ++accepted;
    }
    Require(f.State.Completions.size() == accepted && f.State.Closed == 1, "accepted sends complete before close exactly once");
}

static void TestConcurrentRequestIdentity() {
    struct FRequestResult {
        int Admissions{0};
        int Completions{0};
        ESessionSendAdmission Status{ESessionSendAdmission::NotStarted};
    };
    std::vector<FRequestResult> requests(120);
    std::atomic<int> failedPosts{0};
    Fixture f; f.Start();
    std::vector<std::thread> senders;
    for (int n = 0; n < 4; ++n) senders.emplace_back([&f, &requests, &failedPosts, n] {
        for (int i = 0; i < 30; ++i) {
            const int index = n * 30 + i;
            bool posted = f.Session->RequestSend(8, BUFFER{8},
                [&f, &requests, index](ESessionSendAdmission value) {
                    { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); ++requests[index].Admissions; requests[index].Status = value; f.State.Admissions.push_back(value); }
                    f.State.Changed.notify_all();
                },
                [&f, &requests, index](const BOOST_ERROR_CODE& error, uint64 bytes) {
                    { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); ++requests[index].Completions; f.State.Completions.emplace_back(error, bytes); }
                    f.State.Changed.notify_all();
                });
            if (!posted) ++failedPosts;
        }
        GThreadMemoryCache.FlushToCentral();
    });
    Require(f.Session->RequestStop(), "post stop amid unique requests");
    for (auto& sender : senders) sender.join();
    Require(failedPosts == 0, "post unique requests");
    f.Wait([&] { return f.State.Admissions.size() == 120 && f.State.Closed; }, "unique request results");
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    for (const auto& request : requests) {
        Require(request.Admissions == 1, "each request admission exactly once");
        if (request.Status == ESessionSendAdmission::Accepted)
            Require(request.Completions == 1, "accepted request completes exactly once");
        else {
            Require(request.Status == ESessionSendAdmission::Closing, "unexpected unique request status");
            Require(request.Completions == 0, "rejected request has no completion");
        }
    }
}

static void TestAdmissionAndLifetime() {
    Fixture f;
    Require(f.Send(1, BUFFER(1021, 1)), "post oversized send before start");
    f.Start();
    Require(f.Send(2, BUFFER(1021, 1)), "post too large send");
    f.Wait([&] { return f.State.Admissions.size() == 2; }, "initial admission responses");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Admissions[0] == ESessionSendAdmission::NotStarted &&
            f.State.Admissions[1] == ESessionSendAdmission::InvalidSize, "invalid requests rejected");
        Require(f.State.Completions.empty(), "rejected requests do not complete");
    }
    Require(f.Session->RequestStop(), "post stop with receive pending");
    f.WaitClosed();
    Require(f.Send(3, BUFFER(1021, 1)), "post oversized send after close notification");
    f.Wait([&] { return f.State.Admissions.size() == 3; }, "post-close admission callback");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Admissions[2] == ESessionSendAdmission::Closing &&
            f.State.Completions.empty() && f.State.Closed == 1, "post-close request rejected once");
    }
    WKPTR<UTestSession> weak = f.Session;
    f.Session.reset();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!weak.expired() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    Require(weak.expired(), "session released after pending handlers");
}

static void TestUnownedSessionPostFailure() {
    FTestState state;
    IOCONTEXT serverContext, clientContext;
    TCPACCEPTOR acceptor{serverContext, TCP_ENDPOINT{ASIO::ip::address_v4::loopback(), 0}};
    TCPSOCKET client{clientContext}; client.connect(acceptor.local_endpoint());
    TCPSOCKET socket{serverContext}; acceptor.accept(socket);
    auto core = MakeShared<UCoreInstance>();
    UTestSession session{core, std::move(socket), state};
    int callbacks = 0;
    Require(!session.Start(), "unowned start fails without posting");
    Require(!session.RequestSend(1, BUFFER{1}, [&callbacks](ESessionSendAdmission) { ++callbacks; },
        [&callbacks](const BOOST_ERROR_CODE&, uint64) { ++callbacks; }), "unowned send fails without posting");
    Require(!session.RequestStop(), "unowned stop fails without posting");
    Require(callbacks == 0 && serverContext.poll() == 0, "failed posts do not invoke callbacks");
}

static void TestPendingReceiveLifetime() {
    Fixture f;
    Require(f.Session->Start(), "post start before workers");
    Require(f.Session->RequestStop(), "post stop before workers");
    WKPTR<UTestSession> weak = f.Session;
    f.Session.reset();
    Require(!weak.expired(), "posted work retains session");
    for (int i = 0; i < 2; ++i) f.Workers.emplace_back([&f] {
        f.ServerContext.run(); GThreadMemoryCache.FlushToCentral();
    });
    f.WaitClosed();
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (!weak.expired() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    Require(weak.expired(), "receive cancellation releases last reference");
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(f.State.Closed == 1 && !f.State.CloseReason, "pending receive ends before close notification");
}

static void TestInvalidPacketCancelsQueuedSends() {
    std::vector<int> admissions(2048), completions(2048);
    Fixture f(true); f.Start();
    for (int i = 0; i < 2048; ++i) {
        Require(f.Session->RequestSend(4, BUFFER(508, 5),
            [&f, &admissions, i](ESessionSendAdmission value) {
                { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); ++admissions[i]; f.State.Admissions.push_back(value); }
                f.State.Changed.notify_all();
            },
            [&f, &completions, i](const BOOST_ERROR_CODE& error, uint64 bytes) {
                { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); ++completions[i]; f.State.Completions.emplace_back(error, bytes); }
                f.State.Changed.notify_all();
            }), "post bulk send");
    }
    f.Wait([&] { return f.State.Admissions.size() == 2048; }, "bulk send admissions");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Completions.size() < 2048, "test requires pending sends");
        for (auto value : f.State.Admissions) Require(value == ESessionSendAdmission::Accepted, "bulk admission");
    }
    FPacketHeader invalid{1, 1021};
    std::vector<uint8> bytes(sizeof(invalid)); std::memcpy(bytes.data(), &invalid, sizeof(invalid));
    Write(f.Client, bytes); f.WaitClosed();
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(f.State.CloseReason == ASIO::error::message_size && f.State.Closed == 1, "invalid packet closes session");
    Require(f.State.Completions.size() == 2048, "all admitted sends complete once");
    size_t cancelled = 0;
    for (const auto& result : f.State.Completions)
        if (result.first == ASIO::error::operation_aborted) ++cancelled;
    Require(cancelled != 0, "invalid packet cancels pending sends");
    for (int i = 0; i < 2048; ++i)
        Require(admissions[i] == 1 && completions[i] == 1, "each accepted send notified exactly once");
}

static void TestEOFWithQueuedSends() {
    Fixture f(true); f.Start();
    for (int i = 0; i < 512; ++i)
        Require(f.Send(9, BUFFER(508, 9)), "post send before EOF");
    f.Wait([&] { return f.State.Admissions.size() == 512; }, "EOF bulk admissions");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Completions.size() < 512, "EOF test requires pending sends");
        for (auto result : f.State.Admissions) Require(result == ESessionSendAdmission::Accepted, "EOF bulk accepted");
    }
    BOOST_ERROR_CODE ec; f.Client.shutdown(TCPSOCKET::shutdown_send, ec);
    Require(!ec, "EOF half close with sends pending");
    auto frame = Frame(9, std::vector<uint8>(508, 9));
    std::vector<uint8> expected; expected.reserve(frame.size() * 512);
    for (int i = 0; i < 512; ++i) expected.insert(expected.end(), frame.begin(), frame.end());
    Require(ReadExact(f.Client, expected.size()) == expected, "drain queued sends after EOF");
    f.WaitClosed();
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(!f.State.CloseReason && f.State.Completions.size() == 512, "EOF close after all completions");
    for (const auto& result : f.State.Completions) Require(!result.first && result.second == 512, "EOF send completion");
}

static void TestStopWithQueuedSends() {
    Fixture f(true); f.Start();
    for (int i = 0; i < 512; ++i)
        Require(f.Send(10, BUFFER(508, 10)), "post send before stop");
    f.Wait([&] { return f.State.Admissions.size() == 512; }, "stop bulk admissions");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Completions.size() < 512, "stop test requires pending sends");
        for (auto result : f.State.Admissions) Require(result == ESessionSendAdmission::Accepted, "stop bulk accepted");
    }
    Require(f.Session->RequestStop(), "request stop with sends pending");
    Require(f.Send(11, BUFFER{11}), "post send after stop request");
    f.Wait([&] { return f.State.Admissions.size() == 513; }, "post-stop admission");
    {
        EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
        Require(f.State.Admissions.back() == ESessionSendAdmission::Closing,
            "stop was processed before post-stop admission");
        Require(f.State.Completions.size() < 511,
            "stop was processed while at least one send remained queued");
    }
    auto frame = Frame(10, std::vector<uint8>(508, 10));
    std::vector<uint8> expected; expected.reserve(frame.size() * 512);
    for (int i = 0; i < 512; ++i) expected.insert(expected.end(), frame.begin(), frame.end());
    Require(ReadExact(f.Client, expected.size()) == expected, "drain queued sends after stop");
    f.WaitClosed();
    EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex);
    Require(!f.State.CloseReason && f.State.Closed == 1 && f.State.Completions.size() == 512,
        "stop closes once after all completions");
    for (const auto& result : f.State.Completions) Require(!result.first && result.second == 512, "stop send completion");
}

class UTestSessionListener final : public UListener {
public:
    UTestSessionListener(SHPTR<UCoreInstance> core, IOCONTEXT& context, FUNC<void(TCPSOCKET)> accepted)
        : UListener{std::move(core), context}, _Accepted{std::move(accepted)} {}
protected:
    void HandleAcceptedSocket(TCPSOCKET socket) override { _Accepted(std::move(socket)); }
private:
    FUNC<void(TCPSOCKET)> _Accepted;
};

static void TestListenerConnection() {
    struct FListenerFixture {
        FTestState State;
        IOCONTEXT Context;
        ASIO::executor_work_guard<IOCONTEXT::executor_type> Guard{ASIO::make_work_guard(Context)};
        SHPTR<UCoreInstance> CoreInstance{MakeShared<UCoreInstance>()};
        SHPTR<UTestSession> Session;
        SHPTR<UTestSessionListener> Listener;
        std::vector<std::thread> Workers;
        ~FListenerFixture() {
            BOOST_ERROR_CODE ec;
            if (Listener) Listener->Stop(ec);
            Guard.reset(); Context.stop();
            for (auto& worker : Workers) if (worker.joinable()) worker.join();
            Session.reset(); Listener.reset();
        }
    } f;
    TCPACCEPTOR probe{f.Context, TCP_ENDPOINT{ASIO::ip::address_v4::loopback(), 0}};
    const TCP_ENDPOINT endpoint = probe.local_endpoint(); probe.close();
    f.Listener = MakeShared<UTestSessionListener>(f.CoreInstance, f.Context, [&f](TCPSOCKET socket) {
        auto session = MakeShared<UTestSession>(f.CoreInstance, std::move(socket), f.State);
        const bool started = session->Start();
        { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); f.Session = std::move(session); if (!started) ++f.State.Exceptions; }
        f.State.Changed.notify_all();
    });
    BOOST_ERROR_CODE ec;
    Require(f.Listener->Start(endpoint, [&f](const BOOST_ERROR_CODE&) {
        { EXCLUSIVE_LOCK_GUARD lock(f.State.Mutex); ++f.State.Exceptions; }
        f.State.Changed.notify_all();
    }, ec) && !ec, "listener start");
    for (int i = 0; i < 2; ++i) f.Workers.emplace_back([&f] { f.Context.run(); GThreadMemoryCache.FlushToCentral(); });
    IOCONTEXT clientContext; TCPSOCKET client{clientContext}; client.connect(endpoint);
    Write(client, Frame(55, {5,5}));
    {
        std::unique_lock<SHARED_MUTEX> lock(f.State.Mutex);
        Require(f.State.Changed.wait_for(lock, 5s, [&] { return !f.State.Packets.empty() || f.State.Exceptions; }), "listener session packet");
        Require(!f.State.Exceptions && f.State.Packets[0].first == 55, "listener moves socket to session");
        Require(f.Session->RequestStop(), "listener session stop");
        Require(f.State.Changed.wait_for(lock, 5s, [&] { return f.State.Closed; }), "listener session closed");
    }
}

int main() {
    int result = 0;
    try {
        TestReceive(); std::cout << "PASS fragmented, coalesced, empty, maximum and invalid frames\n";
        TestFragmentedStream(); std::cout << "PASS varied fragmented packet stream\n";
        TestGracefulSend(); std::cout << "PASS send ownership, FIFO, rejection and drain\n";
        TestRepeatedStartAndCallbackStop(); std::cout << "PASS repeated start and callback-requested stop\n";
        TestCompletionCallbackRequestsStop(); std::cout << "PASS send completion requests stop and late send\n";
        TestStopBeforeStart(); std::cout << "PASS stop before start\n";
        TestEOFAndException(); std::cout << "PASS EOF drain and packet exception\n";
        TestEOFWithPartialPacket(); std::cout << "PASS EOF with complete and partial packets\n";
        TestReceiveReset(); std::cout << "PASS receive connection reset\n";
        TestCallbackExceptions(); std::cout << "PASS callback exceptions reported\n";
        TestConcurrentAdmissions(); std::cout << "PASS concurrent send and stop\n";
        TestConcurrentRequestIdentity(); std::cout << "PASS per-request results during concurrent stop\n";
        TestAdmissionAndLifetime(); std::cout << "PASS admission boundaries and session lifetime\n";
        TestUnownedSessionPostFailure(); std::cout << "PASS unowned post failure has no callbacks\n";
        TestPendingReceiveLifetime(); std::cout << "PASS pending receive retains session\n";
        TestInvalidPacketCancelsQueuedSends(); std::cout << "PASS packet error cancels queued sends\n";
        TestEOFWithQueuedSends(); std::cout << "PASS EOF requested with queued sends\n";
        TestStopWithQueuedSends(); std::cout << "PASS stop requested with queued sends\n";
        TestListenerConnection(); std::cout << "PASS UListener accepted socket to USession\n";
    } catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; result = 1; }
    GThreadMemoryCache.FlushToCentral();
    UCoreGlobal::DeleteInstance();
    return result;
}
