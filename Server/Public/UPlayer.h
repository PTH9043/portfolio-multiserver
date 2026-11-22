#pragma once

#include "CoreDefine.h"
#include "UPawn.h"
#include "UPacketHandlerBase.h"
#include "UGameRoom.h"

namespace multiplayer::protocol {
class CSLogin;
class CSChat;
class CSRoomUsers;
class CSRoomList;
class CSJoinRoom;
}

SPACE_H(Server)

class UNicknameRegistry;
class UDatabaseManager;

// 플레이어의 공용 식별·프로필 정보 구조체다.
// 닉네임과 DB 영속 식별자(AccountID·ConnectionLogID)를 함께 보관한다.
struct FPlayerInfo {
	Core::FString Nickname;
	Core::uint64 AccountID{ 0 };
	Core::uint64 ConnectionLogID{ 0 };
};

// 로그인 완료 후 게임 월드에 참여하는 플레이어 엔티티다.
// 공용 패킷 인프라(HandlePacket·SendPacket·RejectPacket·CS_PING)는 UPacketHandlerBase에서 상속받고,
// 이 클래스는 도메인 패킷(채팅·방 목록·방 이동)과 퇴장·DB 로그아웃 정리만 전담한다.
class UPlayer final : public UPawn, public UPacketHandlerBase {
public:
	STATIC_CONSTEXPR Core::uint32 ActorTypeID{ static_cast<Core::uint32>(Core::EActorType::UserStart) };

	explicit UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::SHPTR<Core::USession> session,
		FPlayerInfo info, Core::SHPTR<UNicknameRegistry> nicknameRegistry = nullptr,
		Core::SHPTR<UDatabaseManager> databaseManager = nullptr);
	virtual ~UPlayer() override;
	NO_COPY(UPlayer)

	// RTTI 없이 UPlayer를 판별하기 위한 고정 액터 유형 ID를 돌려준다.
	Core::uint32 GetActorTypeID() const noexcept override { return ActorTypeID; }

	// Core::URoom 브로드캐스트 경로에서 이 플레이어의 연결 세션을 무잠금으로 조회한다.
	Core::SHPTR<Core::USession> GetSession() const noexcept override { return GetBoundSession(); }

	// 생성 시 확정된 불변 플레이어 정보 참조다. 반환한 참조는 이 UPlayer가 살아 있는 동안 유효하다.
	const FPlayerInfo& GetInfo() const noexcept { return _Info; }

	// 연결된 USession의 strand에서 방 입장이 확정됐을 때 소속 방을 약참조로 연결한다.
	void SetGameRoom(const Core::SHPTR<UGameRoom>& gameRoom) noexcept;

	// 연결된 USession의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다. 닉네임 해제·방 퇴장·DB 로그아웃을 요청한다.
	void OnSessionDisconnected() override;

protected:
	// UPacketHandlerBase가 공용 패킷(CS_PING)을 처리한 뒤 위임하는 로그인 완료 도메인 패킷 분기점이다.
	void OnDomainPacket(const Core::FPacketView& packet) override;

	// 프로토콜 위반으로 연결이 닫힐 때 DB 퇴장 사유를 PROTOCOL_ERROR로 기록한다.
	void OnPacketRejected() noexcept override;

private:
	// 연결된 USession의 strand에서 실행되는 도메인 패킷별 처리 함수다.
	void OnPacket(const multiplayer::protocol::CSLogin& packet);
	void OnPacket(const multiplayer::protocol::CSChat& packet);
	void OnPacket(const multiplayer::protocol::CSRoomUsers& packet);
	void OnPacket(const multiplayer::protocol::CSRoomList& packet);
	void OnPacket(const multiplayer::protocol::CSJoinRoom& packet);

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
	Core::ATOMIC_BOOL _NicknameReleased{ false };
	Core::ATOMIC_BOOL _LogoutRecorded{ false };
	Core::WKPTR<UGameRoom> _GameRoom;            // 연결된 USession의 strand에서만 접근한다.
	Core::uint32 _LastRoomID{ 1 };               // 연결된 USession의 strand에서만 접근한다.
	Core::FString _DisconnectReason{ "NORMAL" }; // 연결된 USession의 strand에서만 접근한다.
	bool _RoomTransferPending{ false };          // 연결된 USession의 strand에서만 접근한다.
	bool _Disconnected{ false };                 // 연결된 USession의 strand에서만 접근한다.
};

SPACE_T
