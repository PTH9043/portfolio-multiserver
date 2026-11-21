#pragma once

#include "CoreDefine.h"
#include "UCharacter.h"
#include "UGameRoom.h"

namespace multiplayer::protocol {
class CSLogin;
class CSRegister;
class CSChat;
class CSRoomUsers;
class CSRoomList;
class CSJoinRoom;
class CSPing;
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

// 플레이어가 조종하는 사물이다.
// 세션은 UPawn 컴포넌트(USession)로 부착되며, 로그인 이후 도메인 패킷(채팅·방 목록·방 이동·Ping)과 퇴장 정리를 담당한다.
class UPlayer final : public Core::UCharacter {
public:
	STATIC_CONSTEXPR Core::uint32 ActorTypeID{ static_cast<Core::uint32>(Core::EActorType::UserStart) };

	explicit UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance, FPlayerInfo info,
		Core::SHPTR<UNicknameRegistry> nicknameRegistry = nullptr,
		Core::SHPTR<UDatabaseManager> databaseManager = nullptr);
	virtual ~UPlayer() override;
	NO_COPY(UPlayer)

	// RTTI 없이 UPlayer를 판별하기 위한 고정 액터 유형 ID를 돌려준다.
	Core::uint32 GetActorTypeID() const noexcept override { return ActorTypeID; }

	// 생성 시 확정된 불변 플레이어 정보 참조다. 반환한 참조는 이 UPlayer가 살아 있는 동안 유효하다.
	const FPlayerInfo& GetInfo() const noexcept { return _Info; }

	// 부착된 USession의 strand에서 방 입장이 확정됐을 때 소속 방을 약참조로 연결한다.
	void SetGameRoom(const Core::SHPTR<UGameRoom>& gameRoom) noexcept;

	// 부착된 USession의 strand에서 완성된 패킷 뷰를 받아 도메인 로직으로 가공·전달한다.
	void OnReceivePacket(const Core::FPacketView& packet) override;

	// 부착된 USession의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다. 닉네임 해제 및 소속 방 퇴장을 요청한다.
	void OnSessionDisconnected() override;

private:
	// 모든 패킷이 함께 쓰는 수신 해석 함수다. 실패 시 RejectPacket으로 연결을 닫는다.
	template<class TPacket>
	void HandlePacket(const Core::FPacketView& packet);

	// 부착된 세션으로 서버→클라이언트 패킷을 직렬화·전송한다.
	template<class TPacket>
	void SendPacket(Core::uint16 packetType, const TPacket& packet);

	// 부착된 USession의 strand에서 실행되는 패킷별 처리 함수다.
	void OnPacket(const multiplayer::protocol::CSLogin& packet);
	void OnPacket(const multiplayer::protocol::CSChat& packet);
	void OnPacket(const multiplayer::protocol::CSRoomUsers& packet);
	void OnPacket(const multiplayer::protocol::CSRoomList& packet);
	void OnPacket(const multiplayer::protocol::CSJoinRoom& packet);
	void OnPacket(const multiplayer::protocol::CSPing& packet);

	// 방 이동(Room A 퇴장 완료 → Room B 입장 → 결과 반영·실패 복구)의 세션 strand 후처리 함수다.
	void OnLeaveRoomForTransferCompleted(const Core::SHPTR<UGameRoom>& previousRoom, const Core::SHPTR<UGameRoom>& targetRoom);
	void OnEnterTransferTargetCompleted(const Core::SHPTR<UGameRoom>& previousRoom, const Core::SHPTR<UGameRoom>& targetRoom,
		UGameRoom::EEnterResult result, Core::uint32 userCount);
	void OnRollbackPreviousRoomCompleted(const Core::SHPTR<UGameRoom>& previousRoom,
		UGameRoom::EEnterResult result, Core::uint32 userCount);

	// 예약된 전역 닉네임을 정확히 한 번만 해제한다(퇴장 시 즉시 해제, 소멸자 RAII 안전망 겸용).
	void ReleaseNicknameOnce() noexcept;

	// 프로토콜을 위반한 연결을 Warning으로 기록하고 부착된 세션을 닫는다.
	void RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason);

	FPlayerInfo _Info;
	Core::SHPTR<UNicknameRegistry> _NicknameRegistry;
	Core::SHPTR<UDatabaseManager> _DatabaseManager;
	Core::ATOMIC_BOOL _NicknameReleased{ false };
	Core::ATOMIC_BOOL _LogoutRecorded{ false };
	Core::WKPTR<UGameRoom> _GameRoom;    // 부착된 USession의 strand에서만 접근한다.
	Core::uint32 _LastRoomID{ 1 };       // 부착된 USession의 strand에서만 접근한다.
	Core::FString _DisconnectReason{ "NORMAL" }; // 부착된 USession의 strand에서만 접근한다.
	bool _RoomTransferPending{ false };  // 부착된 USession의 strand에서만 접근한다.
	bool _Disconnected{ false };         // 부착된 USession의 strand에서만 접근한다.
};

SPACE_T
