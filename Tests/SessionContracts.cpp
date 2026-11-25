#include "CoreDefine.h"
#include "Packet.h"
#include "UActor.h"
#include "UComponent.h"
#include "UCoreInstance.h"
#include "UListener.h"
#include "ULogger.h"
#include "UNetworkManager.h"
#include "URoom.h"
#include "URoomManager.h"
#include "USession.h"

#include <boost/asio/connect.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>

using namespace Core;
using namespace std::chrono_literals;

static_assert(PACKET_BUFFER_SIZE == 1024, "session packet buffer must be 1024 bytes");
static_assert(!std::is_copy_constructible_v<FPacketView>, "FPacketView must not be copy constructible");
static_assert(!std::is_copy_assignable_v<FPacketView>, "FPacketView must not be copy assignable");
static_assert(!std::is_move_constructible_v<FPacketView>, "FPacketView must not be move constructible");
static_assert(!std::is_move_assignable_v<FPacketView>, "FPacketView must not be move assignable");
static_assert(std::is_copy_constructible_v<FPacketPayload>, "FPacketPayload must be copy constructible");
static_assert(std::is_move_constructible_v<FPacketPayload>, "FPacketPayload must be move constructible");

namespace {
void Require(const bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}

// 4바이트 리틀 엔디안 헤더와 본문을 하나의 TCP 프레임 바이트 배열로 조립한다.
BUFFER BuildWireFrame(const uint16 type, const uint16 declaredSize, const uint8* body, const uint64 bodyBytes)
{
	FPacketHeader header{};
	header.type = type;
	header.size = declaredSize;

	BUFFER out(PACKET_HEADER_SIZE + bodyBytes);
	std::memcpy(out.data(), &header, PACKET_HEADER_SIZE);
	if (bodyBytes != 0 && body != nullptr)
		std::memcpy(out.data() + PACKET_HEADER_SIZE, body, bodyBytes);
	return out;
}

// ---- Group 2/3 테스트용 파생 타입 (UComponent -> UActor 장착 + TypeID static_cast 검증) ----

class UTestCustomComponent final : public UComponent {
public:
	STATIC_CONSTEXPR uint32 ComponentTypeID{ static_cast<uint32>(EComponentType::UserStart) };
	explicit UTestCustomComponent(SHPTR<UCoreInstance> coreInstance)
		: UComponent{ std::move(coreInstance) } {}
	uint32 GetComponentTypeID() const noexcept override { return ComponentTypeID; }
};

class UTestActor final : public UActor {
public:
	STATIC_CONSTEXPR uint32 ActorTypeID{ static_cast<uint32>(EActorType::UserStart) };
	explicit UTestActor(SHPTR<UCoreInstance> coreInstance)
		: UActor{ std::move(coreInstance) } {}
	uint32 GetActorTypeID() const noexcept override { return ActorTypeID; }
};

class UTestRoom final : public URoom {
public:
	STATIC_CONSTEXPR uint32 RoomTypeID{ static_cast<uint32>(ERoomType::UserStart) };
	explicit UTestRoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint64 roomID, uint32 maxActors, uint32 maxBroadcastTargets = 0)
		: URoom{ std::move(coreInstance), context, roomID, maxActors, maxBroadcastTargets } {}
	uint32 GetRoomTypeID() const noexcept override { return RoomTypeID; }

	bool RunOnRoomStrand(FUNC<void(UTestRoom&)> fn)
	{
		if (!fn)
			return false;
		return PostToRoom([this, fn = std::move(fn)]() mutable { fn(*this); });
	}

	uint64 QueryActorCountOnStrand() const noexcept { return GetActorCount(); }
	uint64 QueryBroadcastTargetCountOnStrand() const noexcept { return GetBroadcastTargetCount(); }
	void SendBroadcastOnStrand(const SHPTR<const BUFFER>& frame, const UActor* excludedActor = nullptr) const
	{
		BroadcastSharedOnStrand(frame, excludedActor);
	}
};

class UOtherRoom final : public URoom {
public:
	STATIC_CONSTEXPR uint32 RoomTypeID{ static_cast<uint32>(ERoomType::UserStart) + 1 };
	explicit UOtherRoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint64 roomID, uint32 maxActors)
		: URoom{ std::move(coreInstance), context, roomID, maxActors } {}
	uint32 GetRoomTypeID() const noexcept override { return RoomTypeID; }
};

// ---- Group 4 네트워크 테스트용 USession(UComponent) + UProbeActor(UActor) ----

struct FSessionProbeState {
	SHARED_MUTEX Lock;
	COND_VARIABLE Changed;
	VECTOR<FPacketPayload> ReceivedPackets;
	uint64 DisconnectedCount{ 0 };
	bool ThrowOnPacket{ false };
	VECTOR<SHPTR<UActor>> AcceptedActors;
};

class UProbeActor final : public UActor {
public:
	STATIC_CONSTEXPR uint32 ActorTypeID{ static_cast<uint32>(EActorType::UserStart) + 1 };
	UProbeActor(SHPTR<UCoreInstance> coreInstance, FSessionProbeState& state)
		: UActor{ std::move(coreInstance) },
		  _State{ state } {}

	uint32 GetActorTypeID() const noexcept override { return ActorTypeID; }

	void OnReceivePacket(const FPacketView& packet) override
	{
		bool shouldThrow{ false };
		{
			EXCLUSIVE_LOCK_GUARD lock{ _State.Lock };
			shouldThrow = _State.ThrowOnPacket;
			if (!shouldThrow)
				_State.ReceivedPackets.push_back(packet.Detach());
		}
		_State.Changed.notify_all();
		if (shouldThrow)
			throw std::runtime_error("probe packet exception");
	}

	void OnSessionDisconnected() override
	{
		{
			EXCLUSIVE_LOCK_GUARD lock{ _State.Lock };
			++_State.DisconnectedCount;
		}
		_State.Changed.notify_all();
	}

private:
	FSessionProbeState& _State;
};

class UProbeListener final : public UListener {
public:
	UProbeListener(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, FSessionProbeState& state, MILLISECONDS idleTimeout)
		: UListener{ std::move(coreInstance), context },
		  _State{ state },
		  _IdleTimeout{ idleTimeout } {}

protected:
	void HandleAcceptedSocket(TCPSOCKET socket) override
	{
		const auto coreInstance = GetCoreInstance();
		const auto networkManager = coreInstance ? coreInstance->GetNetworkManager() : nullptr;
		if (!networkManager)
			return;

		const auto session = MakeShared<USession>(coreInstance, std::move(socket));
		const auto actor = MakeShared<UProbeActor>(coreInstance, _State);
		if (!actor->AddComponent(session))
			return;
		session->SetIdleTimeout(_IdleTimeout);
		{
			EXCLUSIVE_LOCK_GUARD lock{ _State.Lock };
			_State.AcceptedActors.push_back(actor);
		}
		_State.Changed.notify_all();
		networkManager->RequestAddSession(session);
		session->Receive();
	}

private:
	FSessionProbeState& _State;
	MILLISECONDS _IdleTimeout{ 0 };
};

uint16 PickFreeLoopbackPort()
{
	IOCONTEXT io;
	TCPACCEPTOR probe{ io, TCP_ENDPOINT{ ASIO::ip::address_v4::loopback(), 0 } };
	const uint16 port = probe.local_endpoint().port();
	BOOST_ERROR_CODE ignored;
	probe.close(ignored);
	return port;
}

// Group 1: SafeGuard (FPacketView NO_COPY + Detach 불변 복사 + 4스레드 무잠금 동시 읽기 + 경계값 거절)
void TestSafeGuardContracts()
{
	ARRAY<uint8, 32> raw{};
	for (uint64 i = 0; i < raw.size(); ++i)
		raw[i] = static_cast<uint8>(0x30 + i);

	FPacketPayload detached;
	{
		const FPacketView view{ 42, raw.data(), raw.size() };
		Require(view.GetType() == 42 && view.GetSize() == 32 && view.GetData() == raw.data() && !view.IsEmpty(),
			"FPacketView accessors mismatch");
		detached = view.Detach();
	}
	std::memset(raw.data(), 0xCC, raw.size());
	Require(detached.IsValid() && detached.GetType() == 42 && detached.GetSize() == 32,
		"Detached payload metadata mismatch");
	for (uint64 i = 0; i < 32; ++i)
		Require(detached.GetData()[i] == static_cast<uint8>(0x30 + i), "Detached payload corrupted");

	ATOMIC_BOOL ok{ true };
	THREADS workers;
	workers.reserve(4);
	for (int t = 0; t < 4; ++t) {
		workers.emplace_back([payload = detached, &ok]() {
			for (int iter = 0; iter < 4000; ++iter) {
				if (payload.GetType() != 42 || payload.GetSize() != 32) {
					ok.store(false, std::memory_order_relaxed);
					return;
				}
				for (uint64 i = 0; i < 32; ++i) {
					if (payload.GetData()[i] != static_cast<uint8>(0x30 + i)) {
						ok.store(false, std::memory_order_relaxed);
						return;
					}
				}
			}
		});
	}
	for (auto& w : workers)
		w.join();
	Require(ok.load(std::memory_order_acquire), "Concurrent zero-lock payload read failed");

	const FPacketView emptyView{ 9, nullptr, 0 };
	const FPacketPayload emptyDetached = emptyView.Detach();
	Require(emptyDetached.IsValid() && emptyDetached.IsEmpty() && emptyDetached.GetBuffer() == nullptr,
		"Empty payload must be valid and zero-allocated");
	Require(!FPacketPayload::CopyFrom(0, raw.data(), 4).IsValid(), "type=0 must be rejected");
	Require(!FPacketPayload::CopyFrom(7, nullptr, 4).IsValid(), "null body with size>0 must be rejected");
	Require(!FPacketPayload::CopyFrom(7, raw.data(), PACKET_MAX_BODY_SIZE + 1).IsValid(), "oversized payload must be rejected");
	std::cout << "PASS Group 1: SafeGuard FPacketView/FPacketPayload contracts\n";
}

// Group 2: Core::UComponent / Core::UActor 컴파일 타임 static 장착·중복 거절·소멸 후 재장착 계약
void TestComponentStaticContracts()
{
	auto coreInstance = MakeShared<UCoreInstance>();
	Require(coreInstance->Initialize(), "CoreInstance init failed");

	auto actorA = MakeShared<UTestActor>(coreInstance);
	auto actorB = MakeShared<UTestActor>(coreInstance);
	auto comp1 = MakeShared<UTestCustomComponent>(coreInstance);
	auto comp2 = MakeShared<UTestCustomComponent>(coreInstance);

	Require(actorA->AddComponent(comp1) && comp1->HasOwner(), "AddComponent failed");
	Require(!actorA->AddComponent(comp2) && !comp2->HasOwner(), "Duplicate slot must be rejected");
	Require(!actorB->AddComponent(comp1), "Already-owned component must be rejected");
	Require(actorA->GetComponent<UTestCustomComponent>() == comp1, "static_pointer_cast GetComponent mismatch");
	Require(actorA->GetSession() == nullptr, "Actor without session must return nullptr");

	actorA.reset();
	Require(!comp1->HasOwner(), "Owner WKPTR must expire when actorA dies");
	Require(actorB->AddComponent(comp1) && actorB->GetComponent<UTestCustomComponent>() == comp1,
		"Re-attach after owner expiry failed");
	std::cout << "PASS Group 2: UComponent/UActor static contracts\n";
}

// Group 3: URoomManager TypeID static_pointer_cast 조회 + SnapshotRooms 정렬 + 크로스-스트랜드 방 이동 계약
void TestRoomManagerAndTransferContracts()
{
	auto coreInstance = MakeShared<UCoreInstance>();
	Require(coreInstance->Initialize(), "CoreInstance init failed");
	auto& ioContext = coreInstance->GetNetworkManager()->GetIOContext();
	auto roomManager = coreInstance->GetRoomManager();

	auto room2 = MakeShared<UTestRoom>(coreInstance, ioContext, 2, 100);
	auto room1 = MakeShared<UTestRoom>(coreInstance, ioContext, 1, 100);
	auto other = MakeShared<UOtherRoom>(coreInstance, ioContext, 3, 100);
	Require(roomManager->AddRoom(room2) && roomManager->AddRoom(room1) && roomManager->AddRoom(other),
		"AddRoom failed");

	Require(roomManager->FindRoomAs<UTestRoom>(1) == room1, "FindRoomAs matching TypeID failed");
	Require(roomManager->FindRoomAs<UTestRoom>(3) == nullptr, "FindRoomAs mismatched TypeID must return nullptr");
	Require(roomManager->FindRoomAs<UOtherRoom>(3) == other, "FindRoomAs other TypeID failed");

	const auto snapshot = roomManager->SnapshotRooms();
	Require(snapshot.size() == 3 && snapshot[0]->GetRoomID() == 1 && snapshot[1]->GetRoomID() == 2 && snapshot[2]->GetRoomID() == 3,
		"SnapshotRooms must be sorted by roomID ascending");

	auto actor = MakeShared<UTestActor>(coreInstance);
	SHARED_MUTEX lock;
	COND_VARIABLE cv;
	bool transferDone{ false };
	bool transferOk{ false };

	Require(room1->RequestEnter(actor, [&](const bool entered1) {
		if (!entered1 || room1->GetApproximateActorCount() != 1 || actor->GetRoomID() != 1) {
			{ EXCLUSIVE_LOCK_GUARD g{ lock }; transferDone = true; }
			cv.notify_all();
			return;
		}
		room1->RequestLeaveWithCallback(actor, [&](const bool left1) {
			if (!left1 || room1->GetApproximateActorCount() != 0) {
				{ EXCLUSIVE_LOCK_GUARD g{ lock }; transferDone = true; }
				cv.notify_all();
				return;
			}
			room2->RequestEnter(actor, [&](const bool entered2) {
				{
					EXCLUSIVE_LOCK_GUARD g{ lock };
					transferOk = entered2 && room2->GetApproximateActorCount() == 1 && actor->GetRoomID() == 2;
					transferDone = true;
				}
				cv.notify_all();
			});
		});
	}), "RequestEnter failed");

	THREAD worker{ [&ioContext]() { ioContext.run(); } };
	{
		std::unique_lock<SHARED_MUTEX> lk{ lock };
		Require(cv.wait_for(lk, 3s, [&]() { return transferDone; }), "Cross-strand transfer timed out");
		Require(transferOk, "Cross-strand transfer state mismatch");
	}
	coreInstance->GetNetworkManager()->Shutdown();
	worker.join();
	roomManager->ClearRooms();
	std::cout << "PASS Group 3: URoomManager static TypeID & cross-strand transfer contracts\n";
}

// Group 4: USession(UComponent) -> UActor SafeGuard 전달, 1B 분할/2패킷 병합, 예외 격리, 1,020B 초과 차단, 유휴 타임아웃 계약
void TestSessionNetworkAndIdleTimeoutContracts()
{
	auto coreInstance = MakeShared<UCoreInstance>();
	Require(coreInstance->Initialize(), "CoreInstance init failed");
	auto networkManager = coreInstance->GetNetworkManager();
	auto& ioContext = networkManager->GetIOContext();

	FSessionProbeState state;
	auto listener = MakeShared<UProbeListener>(coreInstance, ioContext, state, MILLISECONDS{ 250 });
	BOOST_ERROR_CODE ec;
	const uint16 port = PickFreeLoopbackPort();
	Require(port != 0, "Invalid listener port");
	Require(networkManager->StartListener(listener, TCP_ENDPOINT{ ASIO::ip::address_v4::loopback(), port },
		[](const BOOST_ERROR_CODE&) {}, ec), "StartListener failed");

	THREAD worker{ [&ioContext]() { ioContext.run(); } };
	auto shutdownAll = [&]() {
		networkManager->Shutdown();
		if (worker.joinable())
			worker.join();
		EXCLUSIVE_LOCK_GUARD lk{ state.Lock };
		state.AcceptedActors.clear();
	};

	try {
		IOCONTEXT clientIo;
		const TCP_ENDPOINT ep{ ASIO::ip::address_v4::loopback(), port };

		// 4-1. 1B 분할 전송 + 2패킷 병합 전송 -> 소유자 UActor::OnReceivePacket SafeGuard 전달 검증
		{
			TCPSOCKET sock{ clientIo };
			sock.connect(ep);
			const char msg1[] = "Hello";
			const char msg2[] = "World";
			const BUFFER f1 = BuildWireFrame(11, 5, reinterpret_cast<const uint8*>(msg1), 5);
			const BUFFER f2 = BuildWireFrame(12, 5, reinterpret_cast<const uint8*>(msg2), 5);
			for (uint64 i = 0; i < f1.size(); ++i)
				ASIO::write(sock, ASIO::buffer(f1.data() + i, 1));
			BUFFER combined;
			combined.insert(combined.end(), f1.begin(), f1.end());
			combined.insert(combined.end(), f2.begin(), f2.end());
			ASIO::write(sock, ASIO::buffer(combined.data(), combined.size()));

			std::unique_lock<SHARED_MUTEX> lk{ state.Lock };
			Require(state.Changed.wait_for(lk, 2s, [&]() { return state.ReceivedPackets.size() >= 3; }),
				"Fragmented/coalesced packets not received");
			Require(state.ReceivedPackets[0].GetType() == 11 && state.ReceivedPackets[2].GetType() == 12,
				"Packet types mismatch");
			Require(!state.AcceptedActors.empty() && state.AcceptedActors.back()->GetSession() != nullptr,
				"Actor::GetSession must return mounted USession component");
			sock.close();
		}

		// 4-2. 유휴 타임아웃(250ms) 자동 차단 검증
		{
			uint64 prevDisc{ 0 };
			{
				SHARED_LOCK_GUARD lk{ state.Lock };
				prevDisc = state.DisconnectedCount;
			}
			TCPSOCKET idleSock{ clientIo };
			idleSock.connect(ep);
			ARRAY<uint8, 16> sink{};
			BOOST_ERROR_CODE readEc;
			const uint64 n = static_cast<uint64>(idleSock.read_some(ASIO::buffer(sink.data(), sink.size()), readEc));
			Require(n == 0 && static_cast<bool>(readEc), "Idle client was not disconnected by idle timeout");

			std::unique_lock<SHARED_MUTEX> lk{ state.Lock };
			Require(state.Changed.wait_for(lk, 2s, [&]() { return state.DisconnectedCount > prevDisc; }),
				"Actor OnSessionDisconnected not fired on idle timeout");
		}

		// 4-3. 1,020B 초과 헤더 즉시 차단 검증
		{
			TCPSOCKET badSock{ clientIo };
			badSock.connect(ep);
			const BUFFER badHeader = BuildWireFrame(11, static_cast<uint16>(PACKET_MAX_BODY_SIZE + 1), nullptr, 0);
			ASIO::write(badSock, ASIO::buffer(badHeader.data(), badHeader.size()));
			ARRAY<uint8, 16> sink{};
			BOOST_ERROR_CODE readEc;
			const uint64 n = static_cast<uint64>(badSock.read_some(ASIO::buffer(sink.data(), sink.size()), readEc));
			Require(n == 0 && static_cast<bool>(readEc), "Oversized header did not close connection");
		}

		// 4-4. ProcessingData 예외 격리 검증
		{
			{
				EXCLUSIVE_LOCK_GUARD lk{ state.Lock };
				state.ThrowOnPacket = true;
			}
			TCPSOCKET excSock{ clientIo };
			excSock.connect(ep);
			const BUFFER ping = BuildWireFrame(77, 0, nullptr, 0);
			ASIO::write(excSock, ASIO::buffer(ping.data(), ping.size()));
			ARRAY<uint8, 16> sink{};
			BOOST_ERROR_CODE readEc;
			const uint64 n = static_cast<uint64>(excSock.read_some(ASIO::buffer(sink.data(), sink.size()), readEc));
			Require(n == 0 && static_cast<bool>(readEc), "Exception in ProcessingData did not close session");
			{
				EXCLUSIVE_LOCK_GUARD lk{ state.Lock };
				state.ThrowOnPacket = false;
			}
		}

		// 4-5. 예외 이후에도 서버 생존 확인
		{
			uint64 beforeCount{ 0 };
			{
				SHARED_LOCK_GUARD lk{ state.Lock };
				beforeCount = static_cast<uint64>(state.ReceivedPackets.size());
			}
			TCPSOCKET healthySock{ clientIo };
			healthySock.connect(ep);
			const BUFFER okFrame = BuildWireFrame(88, 0, nullptr, 0);
			ASIO::write(healthySock, ASIO::buffer(okFrame.data(), okFrame.size()));
			std::unique_lock<SHARED_MUTEX> lk{ state.Lock };
			Require(state.Changed.wait_for(lk, 2s, [&]() { return state.ReceivedPackets.size() > beforeCount; }),
				"Server did not survive previous session exception");
			healthySock.close();
		}
	}
	catch (...) {
		shutdownAll();
		throw;
	}
	shutdownAll();
	std::cout << "PASS Group 4: USession(UComponent) -> UActor framing, idle timeout, oversize reject, exception isolation\n";
}

// Group 5: URoom 브로드캐스트 대상 분리(_BroadcastTargets) 및 swap-and-pop 정합성 계약
void TestRoomBroadcastTargetSeparationContracts()
{
	auto coreInstance = MakeShared<UCoreInstance>();
	Require(coreInstance->Initialize(), "CoreInstance init failed");
	auto networkManager = coreInstance->GetNetworkManager();
	auto& ioContext = networkManager->GetIOContext();

	FSessionProbeState state;
	auto listener = MakeShared<UProbeListener>(coreInstance, ioContext, state, MILLISECONDS{ 0 });
	BOOST_ERROR_CODE ec;
	const uint16 port = PickFreeLoopbackPort();
	Require(port != 0, "Invalid listener port");
	Require(networkManager->StartListener(listener, TCP_ENDPOINT{ ASIO::ip::address_v4::loopback(), port },
		[](const BOOST_ERROR_CODE&) {}, ec), "StartListener failed");

	auto room = MakeShared<UTestRoom>(coreInstance, ioContext, 50, 32);
	THREAD worker{ [&ioContext]() { ioContext.run(); } };
	auto shutdownAll = [&]() {
		networkManager->Shutdown();
		if (worker.joinable())
			worker.join();
		EXCLUSIVE_LOCK_GUARD lk{ state.Lock };
		state.AcceptedActors.clear();
	};

	auto runRoomSync = [&](FUNC<void(UTestRoom&)> fn) {
		SHARED_MUTEX lkMutex;
		COND_VARIABLE cv;
		bool done{ false };
		Require(room->RunOnRoomStrand([&](UTestRoom& r) {
			fn(r);
			{
				EXCLUSIVE_LOCK_GUARD g{ lkMutex };
				done = true;
			}
			cv.notify_all();
		}), "RunOnRoomStrand failed");
		std::unique_lock<SHARED_MUTEX> lk{ lkMutex };
		Require(cv.wait_for(lk, 3s, [&]() { return done; }), "Room strand op timed out");
	};

	auto readExactFrameType = [](TCPSOCKET& sock) -> uint16 {
		FPacketHeader hdr{};
		ASIO::read(sock, ASIO::buffer(&hdr, PACKET_HEADER_SIZE));
		if (hdr.size > 0) {
			BUFFER body(hdr.size);
			ASIO::read(sock, ASIO::buffer(body.data(), body.size()));
		}
		return hdr.type;
	};

	try {
		IOCONTEXT clientIo;
		const TCP_ENDPOINT ep{ ASIO::ip::address_v4::loopback(), port };

		TCPSOCKET c1{ clientIo };
		TCPSOCKET c2{ clientIo };
		TCPSOCKET c3{ clientIo };
		c1.connect(ep);
		c2.connect(ep);
		c3.connect(ep);

		VECTOR<SHPTR<UActor>> players;
		{
			std::unique_lock<SHARED_MUTEX> lk{ state.Lock };
			Require(state.Changed.wait_for(lk, 3s, [&]() { return state.AcceptedActors.size() >= 3; }),
				"Expected 3 accepted players");
			players = state.AcceptedActors;
		}
		auto npc1 = MakeShared<UTestActor>(coreInstance);
		auto npc2 = MakeShared<UTestActor>(coreInstance);
		auto npc3 = MakeShared<UTestActor>(coreInstance);

		// 비세션 액터 3개 + 세션 액터 3개 혼합 입장
		VECTOR<SHPTR<UActor>> enterOrder{ npc1, players[0], npc2, players[1], players[2], npc3 };
		for (const auto& a : enterOrder) {
			SHARED_MUTEX m;
			COND_VARIABLE cv;
			bool entered{ false };
			bool done{ false };
			Require(room->RequestEnter(a, [&](bool ok) {
				{
					EXCLUSIVE_LOCK_GUARD g{ m };
					entered = ok;
					done = true;
				}
				cv.notify_all();
			}), "RequestEnter failed");
			std::unique_lock<SHARED_MUTEX> lk{ m };
			Require(cv.wait_for(lk, 3s, [&]() { return done; }) && entered, "Actor enter failed");
		}

		// 전체 액터 6명 vs 브로드캐스트 대상 3명 분리 확인 + excludedActor(players[0]) 제외 브로드캐스트 검증
		const char msg1[] = "B1";
		const auto frame1 = USession::MakePacketBuffer(msg1, 2, 91);
		Require(frame1 != nullptr, "MakePacketBuffer failed");
		uint64 totalCount{ 0 };
		uint64 targetCount{ 0 };
		runRoomSync([&](UTestRoom& r) {
			totalCount = r.QueryActorCountOnStrand();
			targetCount = r.QueryBroadcastTargetCountOnStrand();
			r.SendBroadcastOnStrand(frame1, players[0].get());
		});
		Require(totalCount == 6 && targetCount == 3, "Initial actor/broadcast target count mismatch");
		Require(readExactFrameType(c2) == 91, "c2 missed broadcast 91");
		Require(readExactFrameType(c3) == 91, "c3 missed broadcast 91");

		// 첫 번째 세션 액터(players[0])와 비세션 액터(npc2) 퇴장 -> _BroadcastTargets swap-and-pop(players[2] -> slot 0) 검증
		auto leaveSync = [&](const SHPTR<UActor>& a) {
			SHARED_MUTEX m;
			COND_VARIABLE cv;
			bool left{ false };
			bool done{ false };
			Require(room->RequestLeaveWithCallback(a, [&](bool ok) {
				{
					EXCLUSIVE_LOCK_GUARD g{ m };
					left = ok;
					done = true;
				}
				cv.notify_all();
			}), "RequestLeaveWithCallback failed");
			std::unique_lock<SHARED_MUTEX> lk{ m };
			Require(cv.wait_for(lk, 3s, [&]() { return done; }) && left, "Actor leave failed");
		};

		leaveSync(players[0]);
		leaveSync(npc2);

		const char msg2[] = "B2";
		const auto frame2 = USession::MakePacketBuffer(msg2, 2, 92);
		runRoomSync([&](UTestRoom& r) {
			totalCount = r.QueryActorCountOnStrand();
			targetCount = r.QueryBroadcastTargetCountOnStrand();
			r.SendBroadcastOnStrand(frame2);
		});
		Require(totalCount == 4 && targetCount == 2, "Post-leave actor/broadcast target count mismatch");
		Require(readExactFrameType(c2) == 92, "c2 missed broadcast 92");
		Require(readExactFrameType(c3) == 92, "c3 missed broadcast 92");

		// swap-and-pop으로 slot 0에 이동한 players[2] 퇴장 후 마지막 players[1]만 수신 검증
		leaveSync(players[2]);
		const char msg3[] = "B3";
		const auto frame3 = USession::MakePacketBuffer(msg3, 2, 93);
		runRoomSync([&](UTestRoom& r) {
			totalCount = r.QueryActorCountOnStrand();
			targetCount = r.QueryBroadcastTargetCountOnStrand();
			r.SendBroadcastOnStrand(frame3);
		});
		Require(totalCount == 3 && targetCount == 1, "After removing swapped target, count mismatch");
		Require(readExactFrameType(c2) == 93, "c2 missed broadcast 93");

		// 마지막 세션 액터 퇴장 -> 비세션 액터 2개만 남고 브로드캐스트 대상 0명
		leaveSync(players[1]);
		runRoomSync([&](UTestRoom& r) {
			totalCount = r.QueryActorCountOnStrand();
			targetCount = r.QueryBroadcastTargetCountOnStrand();
		});
		Require(totalCount == 2 && targetCount == 0, "Final NPC-only room count mismatch");

		c1.close();
		c2.close();
		c3.close();
	}
	catch (...) {
		shutdownAll();
		throw;
	}
	shutdownAll();
	std::cout << "PASS Group 5: URoom broadcast target separation, excludedActor & swap-and-pop contracts\n";
}

// Group 6: URoom 이중 정원 격리(플레이어 상한 vs 비세션 오브젝트 상한) 극한 상황 방어 계약
void TestRoomDualCapacityIsolationContracts()
{
	auto coreInstance = MakeShared<UCoreInstance>();
	Require(coreInstance->Initialize(), "CoreInstance init failed");
	auto networkManager = coreInstance->GetNetworkManager();
	auto& ioContext = networkManager->GetIOContext();

	// 전체 정원 10, 통신 액터(플레이어) 정원 2 -> 비세션 액터(몬스터·트랙 오브젝트) 전용 상한 8
	auto room = MakeShared<UTestRoom>(coreInstance, ioContext, 60, 10, 2);
	Require(room->GetMaxActorCount() == 10 && room->GetMaxBroadcastTargetCount() == 2,
		"Dual capacity getter mismatch");

	THREAD worker{ [&ioContext]() { ioContext.run(); } };
	auto shutdownAll = [&]() {
		networkManager->Shutdown();
		if (worker.joinable())
			worker.join();
	};

	auto enterSync = [&](const SHPTR<UActor>& a) -> bool {
		SHARED_MUTEX m;
		COND_VARIABLE cv;
		bool entered{ false };
		bool done{ false };
		Require(room->RequestEnter(a, [&](bool ok) {
			{
				EXCLUSIVE_LOCK_GUARD g{ m };
				entered = ok;
				done = true;
			}
			cv.notify_all();
		}), "RequestEnter failed");
		std::unique_lock<SHARED_MUTEX> lk{ m };
		Require(cv.wait_for(lk, 3s, [&]() { return done; }), "Enter timed out");
		return entered;
	};

	auto leaveSync = [&](const SHPTR<UActor>& a) -> bool {
		SHARED_MUTEX m;
		COND_VARIABLE cv;
		bool left{ false };
		bool done{ false };
		Require(room->RequestLeaveWithCallback(a, [&](bool ok) {
			{
				EXCLUSIVE_LOCK_GUARD g{ m };
				left = ok;
				done = true;
			}
			cv.notify_all();
		}), "RequestLeaveWithCallback failed");
		std::unique_lock<SHARED_MUTEX> lk{ m };
		Require(cv.wait_for(lk, 3s, [&]() { return done; }), "Leave timed out");
		return left;
	};

	auto makePlayer = [&]() -> SHPTR<UActor> {
		auto a = MakeShared<UTestActor>(coreInstance);
		auto s = MakeShared<USession>(coreInstance, TCPSOCKET{ ioContext });
		Require(a->AddComponent(s), "AddComponent(USession) failed");
		return a;
	};

	try {
		// 6-1. 비세션 액터 8개 폭주 스폰 -> 8개까지 허용, 9번째는 플레이어 슬롯 보호를 위해 차단
		VECTOR<SHPTR<UActor>> objects;
		for (uint64 i = 0; i < 8; ++i) {
			auto obj = MakeShared<UTestActor>(coreInstance);
			Require(enterSync(obj), "Non-session actor within quota rejected");
			objects.push_back(std::move(obj));
		}
		auto overflowObj = MakeShared<UTestActor>(coreInstance);
		Require(!enterSync(overflowObj), "9th non-session actor must not steal reserved player slots");
		Require(room->GetApproximateActorCount() == 8 && room->GetApproximateBroadcastTargetCount() == 0,
			"Counts mismatch after non-session fill");

		// 6-2. 비세션 상한이 꽉 찬 상태에서도 예약된 플레이어 2명은 정상 입장
		auto p1 = makePlayer();
		auto p2 = makePlayer();
		auto p3 = makePlayer();
		Require(enterSync(p1), "Player 1 starved by non-session actors");
		Require(enterSync(p2), "Player 2 starved by non-session actors");
		Require(room->GetApproximateActorCount() == 10 && room->GetApproximateBroadcastTargetCount() == 2,
			"Counts mismatch after players entered");

		// 6-3. 비세션 4개가 퇴장해 전체 여유 공간이 생겨도, 플레이어 상한(2명)을 넘는 3번째 플레이어는 하드 차단
		for (uint64 i = 0; i < 4; ++i)
			Require(leaveSync(objects[i]), "Object leave failed");
		Require(room->GetApproximateActorCount() == 6 && room->GetApproximateBroadcastTargetCount() == 2,
			"Counts mismatch after object leave");
		Require(!enterSync(p3), "3rd player must be rejected by maxBroadcastTargetCount even when actor slots are free");

		// 6-4. 기존 플레이어 1명이 퇴장하면 3번째 플레이어가 정상 입장
		Require(leaveSync(p1), "Player 1 leave failed");
		Require(enterSync(p3), "Player 3 should enter after player slot freed");
		Require(room->GetApproximateActorCount() == 6 && room->GetApproximateBroadcastTargetCount() == 2,
			"Final dual-capacity counts mismatch");
	}
	catch (...) {
		shutdownAll();
		throw;
	}
	shutdownAll();
	std::cout << "PASS Group 6: URoom dual capacity isolation (player cap vs non-session object cap) contracts\n";
}
}

int main()
{
	try {
		UCoreGlobal::GetInstance();
		TestSafeGuardContracts();
		TestComponentStaticContracts();
		TestRoomManagerAndTransferContracts();
		TestSessionNetworkAndIdleTimeoutContracts();
		TestRoomBroadcastTargetSeparationContracts();
		TestRoomDualCapacityIsolationContracts();
		std::cout << "RESULT 6/6 SessionContracts groups passed\n";
		return 0;
	}
	catch (const std::exception& ex) {
		std::cerr << "[FAIL] SessionContracts: " << ex.what() << '\n';
		return 1;
	}
}
