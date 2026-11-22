#pragma once

#include "CoreDefine.h"
#include "UPawn.h"
#include "UGameRoom.h"

namespace multiplayer::protocol {
class CSRegister;
class CSLogin;
class CSChat;
class CSRoomUsers;
class CSRoomList;
class CSJoinRoom;
}

SPACE_H(Server)

class UNicknameRegistry;
class UDatabaseManager;
enum class EDbRegisterStatus : Core::uint8;
struct FDbLoginOutcome;

// 플레이어의 공용 식별·프로필 정보 구조체다.
// 닉네임과 DB 영속 식별자(AccountID·ConnectionLogID)를 함께 보관한다.
struct FPlayerInfo {
	Core::FString Nickname;
	Core::uint64 AccountID{ 0 };
	Core::uint64 ConnectionLogID{ 0 };
};

// 서버에 접속한 플레이어 엔티티다.
// Core::UActor에 UServerSession(UComponent)이 장착되고, Server::UPawn에서 공용 패킷 인프라를 상속받는다.
// 로그인 전에는 CS_REGISTER · CS_LOGIN만 허용하고, 기본 방 입장 확정 후 채팅·방 이동·퇴장·DB 로그아웃을 담당한다.
class UPlayer final : public UPawn {
public:
	STATIC_CONSTEXPR Core::uint32 ActorTypeID{ static_cast<Core::uint32>(Core::EActorType::UserStart) };
	using FDisconnectCleanupCallback = Core::FUNC<void(const Core::SHPTR<UPlayer>&)>;

	explicit UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance,
		Core::SHPTR<UNicknameRegistry> nicknameRegistry = nullptr,
		Core::SHPTR<UDatabaseManager> databaseManager = nullptr,
		Core::FString remoteIp = Core::FString{ "127.0.0.1" },
		FDisconnectCleanupCallback onDisconnectedCleanup = nullptr);
	virtual ~UPlayer() override;
	NO_COPY(UPlayer)

	// RTTI 없이 UPlayer를 판별하기 위한 고정 액터 유형 ID를 돌려준다.
	Core::uint32 GetActorTypeID() const noexcept override { return ActorTypeID; }

	// 확정된 플레이어 정보 참조다. 반환한 참조는 이 UPlayer가 살아 있는 동안 유효하다.
	const FPlayerInfo& GetInfo() const noexcept { return _Info; }

	// 장착된 USession의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다.
	// 닉네임 해제 · 소속 방 퇴장 · DB 로그아웃 · 리스너 활성 목록 제거를 수행한다.
	void OnSessionDisconnected() override;

protected:
	// UPawn이 공용 패킷(CS_PING)을 처리한 뒤 위임하는 도메인 패킷 분기점이다.
	void OnDomainPacket(const Core::FPacketView& packet) override;

	Core::STRINGVIEW GetPawnTypeName() const noexcept override { return "UPlayer"; }

	// 프로토콜 위반으로 연결이 닫힐 때 DB 퇴장 사유를 PROTOCOL_ERROR로 기록한다.
	void OnPacketRejected() noexcept override;

private:
	// 장착된 USession의 strand에서 실행되는 패킷별 처리 함수다.
	void OnPacket(const multiplayer::protocol::CSRegister& packet);
	void OnPacket(const multiplayer::protocol::CSLogin& packet);
	void OnPacket(const multiplayer::protocol::CSChat& packet);
	void OnPacket(const multiplayer::protocol::CSRoomUsers& packet);
	void OnPacket(const multiplayer::protocol::CSRoomList& packet);
	void OnPacket(const multiplayer::protocol::CSJoinRoom& packet);

	// DB 전용 스레드에서 완료된 회원가입/로그인 결과를 이 Session의 strand에서 반영한다.
	void OnDbRegisterCompleted(EDbRegisterStatus status);
	void OnDbLoginCompleted(const FDbLoginOutcome& outcome);

	// 닉네임 예약 + 기본 방 비동기 입장 요청 및 결과 반영 경로다.
	void BeginInitialRoomEnter(Core::FString nickname, Core::uint64 accountId, Core::uint64 connectionLogId);
	void OnEnterInitialRoomCompleted(const Core::SHPTR<UGameRoom>& gameRoom,
		UGameRoom::EEnterResult result, Core::uint32 userCount);

	// 방 이동(Room A 퇴장 완료 → Room B 입장 → 결과 반영·실패 복구)의 세션 strand 후처리 함수다.
	void OnLeaveRoomForTransferCompleted(const Core::SHPTR<UGameRoom>& previousRoom, const Core::SHPTR<UGameRoom>& targetRoom);
	void OnEnterTransferTargetCompleted(const Core::SHPTR<UGameRoom>& previousRoom, const Core::SHPTR<UGameRoom>& targetRoom,
		UGameRoom::EEnterResult result, Core::uint32 userCount);
	void OnRollbackPreviousRoomCompleted(const Core::SHPTR<UGameRoom>& previousRoom,
		UGameRoom::EEnterResult result, Core::uint32 userCount);

	// 예약된 전역 닉네임을 정확히 한 번만 해제한다(퇴장 시 즉시 해제, 소멸자 RAII 안전망 겸용).
	void ReleaseNicknameOnce() noexcept;

	FPlayerInfo _Info;
	Core::SHPTR<UNicknameRegistry> _NicknameRegistry;
	Core::SHPTR<UDatabaseManager> _DatabaseManager;
	Core::FString _RemoteIp;
	FDisconnectCleanupCallback _OnDisconnectedCleanup;
	Core::ATOMIC_BOOL _NicknameReserved{ false };
	Core::ATOMIC_BOOL _LogoutRecorded{ false };
	Core::WKPTR<UGameRoom> _GameRoom;            // 장착된 USession의 strand에서만 접근한다.
	Core::uint32 _LastRoomID{ 1 };               // 장착된 USession의 strand에서만 접근한다.
	Core::FString _DisconnectReason{ "NORMAL" }; // 장착된 USession의 strand에서만 접근한다.
	bool _LoggedIn{ false };                     // 장착된 USession의 strand에서만 접근한다.
	bool _LoginPending{ false };                 // 장착된 USession의 strand에서만 접근한다.
	bool _RegisterPending{ false };              // 장착된 USession의 strand에서만 접근한다.
	bool _RoomTransferPending{ false };          // 장착된 USession의 strand에서만 접근한다.
	bool _Disconnected{ false };                 // 장착된 USession의 strand에서만 접근한다.
};

SPACE_T
