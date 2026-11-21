#include "CoreDefine.h"
#include "Packet.h"
#include "UComponent.h"
#include "UCoreInstance.h"
#include "UListener.h"
#include "ULogger.h"
#include "UNetworkManager.h"
#include "UPawn.h"
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

// ---- Group 2/4 테스트용 파생 타입 (TypeID + static_cast 계약 검증) ----

class UTestCustomComponent final : public UComponent {
public:
	STATIC_CONSTEXPR uint32 ComponentTypeID{ static_cast<uint32>(EComponentType::UserStart) };
	explicit UTestCustomComponent(SHPTR<UCoreInstance> coreInstance)
		: UComponent{ std::move(coreInstance) } {}
	uint32 GetComponentTypeID() const noexcept override { return ComponentTypeID; }
};

class UTestPawn final : public UPawn {
public:
	STATIC_CONSTEXPR uint32 ActorTypeID{ static_cast<uint32>(EActorType::UserStart) };
	explicit UTestPawn(SHPTR<UCoreInstance> coreInstance)
		: UPawn{ std::move(coreInstance) } {}
	uint32 GetActorTypeID() const noexcept override { return ActorTypeID; }
};

class UTestRoom final : public URoom {
public:
	STATIC_CONSTEXPR uint32 RoomTypeID{ static_cast<uint32>(ERoomType::UserStart) };
	explicit UTestRoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint64 roomID, uint32 maxActors)
		: URoom{ std::move(coreInstance), context, roomID, maxActors } {}
	uint32 GetRoomTypeID() const noexcept override { return RoomTypeID; }
};

class UOtherRoom final : public URoom {
public:
	STATIC_CONSTEXPR uint32 RoomTypeID{ static_cast<uint32>(ERoomType::UserStart) + 1 };
	explicit UOtherRoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint64 roomID, uint32 maxActors)
		: URoom{ std::move(coreInstance), context, roomID, maxActors } {}
	uint32 GetRoomTypeID() const noexcept override { return RoomTypeID; }
};

// ---- Group 1 네트워크 테스트용 세션·리스너 ----

struct FSessionProbeState {
	SHARED_MUTEX Lock;
	COND_VARIABLE Changed;
	VECTOR<FPacketPayload> ReceivedPackets;
	uint64 DisconnectedCount{ 0 };
	bool ThrowOnPacket{ false };
	SHPTR<USession> LastAcceptedSession;
};

class UProbeSession final : public USession {
public:
	UProbeSession(SHPTR<UCoreInstance> coreInstance, TCPSOCKET socket, FSessionProbeState& state)
		: USession{ std::move(coreInstance), std::move(socket) },
		  _State{ state } {}

protected:
	void OnUnboundPacket(const FPacketView& packet) override
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

	void OnDisconnected() override
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

		const auto session = MakeShared<UProbeSession>(coreInstance, std::move(socket), _State);
		session->SetIdleTimeout(_IdleTimeout);
		{
			EXCLUSIVE_LOCK_GUARD lock{ _State.Lock };
			_State.LastAcceptedSession = session;
		}
		_State.Changed.notify_all();
		networkManager->RequestAddSession(session);
		session->Receive();
	}

private:
	FSessionProbeState& _State;
	MILLISECONDS _IdleTimeout{ 0 };
};

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

// Group 2: UComponent / UPawn 컴파일 타임 static 부착·중복 거절·소멸 후 재부착 계약
void TestComponentStaticContracts()
{
	auto coreInstance = MakeShared<UCoreInstance>();
	Require(coreInstance->Initialize(), "CoreInstance init failed");

	auto pawnA = MakeShared<UTestPawn>(coreInstance);
	auto pawnB = MakeShared<UTestPawn>(coreInstance);
	auto comp1 = MakeShared<UTestCustomComponent>(coreInstance);
	auto comp2 = MakeShared<UTestCustomComponent>(coreInstance);

	Require(pawnA->AddComponent(comp1) && comp1->HasOwner(), "AddComponent failed");
	Require(!pawnA->AddComponent(comp2) && !comp2->HasOwner(), "Duplicate slot must be rejected");
	Require(!pawnB->AddComponent(comp1), "Already-owned component must be rejected");
	Require(pawnA->GetComponent<UTestCustomComponent>() == comp1, "static_pointer_cast GetComponent mismatch");
	Require(pawnA->GetSession() == nullptr, "Pawn without session must return nullptr");

	pawnA.reset();
	Require(!comp1->HasOwner(), "Owner WKPTR must expire when pawnA dies");
	Require(pawnB->AddComponent(comp1) && pawnB->GetComponent<UTestCustomComponent>() == comp1,
		"Re-attach after owner expiry failed");
	std::cout << "PASS Group 2: UComponent/UPawn static contracts\n";
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

	// TypeID 일치 시 static_pointer_cast 성공, 불일치 시 nullptr 반환
	Require(roomManager->FindRoomAs<UTestRoom>(1) == room1, "FindRoomAs matching TypeID failed");
	Require(roomManager->FindRoomAs<UTestRoom>(3) == nullptr, "FindRoomAs mismatched TypeID must return nullptr");
	Require(roomManager->FindRoomAs<UOtherRoom>(3) == other, "FindRoomAs other TypeID failed");

	const auto snapshot = roomManager->SnapshotRooms();
	Require(snapshot.size() == 3 && snapshot[0]->GetRoomID() == 1 && snapshot[1]->GetRoomID() == 2 && snapshot[2]->GetRoomID() == 3,
		"SnapshotRooms must be sorted by roomID ascending");

	// 크로스-스트랜드 입·퇴장 및 무잠금 근사 인원 수 검증
	auto actor = MakeShared<UTestPawn>(coreInstance);
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

// Group 4: USession TCP 분할/병합, 예외 격리, 1,020B 초과 차단, 1 MiB 백프레셔, 유휴 타임아웃 계약
uint16 PickFreeLoopbackPort()
{
	IOCONTEXT io;
	TCPACCEPTOR probe{ io, TCP_ENDPOINT{ ASIO::ip::address_v4::loopback(), 0 } };
	const uint16 port = probe.local_endpoint().port();
	BOOST_ERROR_CODE ignored;
	probe.close(ignored);
	return port;
}

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
	};

	try {
		IOCONTEXT clientIo;
		const TCP_ENDPOINT ep{ ASIO::ip::address_v4::loopback(), port };

		// 4-1. 1B 분할 전송 + 2패킷 병합 전송 + 타이머 리셋 유지
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
			sock.close();
		}

		// 4-2. 유휴 타임아웃(250ms) 자동 차단 검증: 아무것도 안 보내면 서버가 연결을 닫는다.
		{
			uint64 prevDisc{ 0 };
			{
				SHARED_LOCK_GUARD lk{ state.Lock };
				prevDisc = state.DisconnectedCount;
			}
			TCPSOCKET idleSock{ clientIo };
			idleSock.connect(ep);
			// 소켓에서 EOF를 기다린다(250ms 타임아웃이므로 2초 내에 서버가 끊어야 한다).
			ARRAY<uint8, 16> sink{};
			BOOST_ERROR_CODE readEc;
			const uint64 n = static_cast<uint64>(idleSock.read_some(ASIO::buffer(sink.data(), sink.size()), readEc));
			Require(n == 0 && static_cast<bool>(readEc), "Idle client was not disconnected by idle timeout");

			std::unique_lock<SHARED_MUTEX> lk{ state.Lock };
			Require(state.Changed.wait_for(lk, 2s, [&]() { return state.DisconnectedCount > prevDisc; }),
				"Server session OnDisconnected not fired on idle timeout");
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

		// 4-4. ProcessingData 예외 격리 검증 (해당 연결만 종료, 서버 워커 생존)
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

		// 4-5. 예외 이후에도 서버가 살아 있어 새 연결의 패킷을 정상 수신하는지 확인
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
	std::cout << "PASS Group 4: USession framing, idle timeout, oversize reject, exception isolation\n";
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
		std::cout << "RESULT 4/4 SessionContracts groups passed\n";
		return 0;
	}
	catch (const std::exception& ex) {
		std::cerr << "[FAIL] SessionContracts: " << ex.what() << '\n';
		return 1;
	}
}
