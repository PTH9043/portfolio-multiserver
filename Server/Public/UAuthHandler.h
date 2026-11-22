#pragma once

#include "CoreDefine.h"
#include "UObject.h"
#include "UPacketHandlerBase.h"
#include "UGameRoom.h"

namespace multiplayer::protocol {
class CSLogin;
class CSRegister;
}

SPACE_H(Server)

class UPlayer;
class UNicknameRegistry;
class UDatabaseManager;
enum class EDbRegisterStatus : Core::uint8;
struct FDbLoginOutcome;

// 로그인 전 미인증 연결 전용 경량 핸들러다.
// 게임 엔티티(UPawn/UPlayer)나 컴포넌트 맵을 미리 만들지 않아 미인증 대량 접속(DoS) 공격면을 최소화하며,
// CS_REGISTER · CS_LOGIN 인증 및 기본 방 입장이 확정되는 순간에만 USession::SetHandler(player)로 원자 전환한다.
class UAuthHandler final : public Core::UObject, public UPacketHandlerBase {
public:
	explicit UAuthHandler(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::SHPTR<Core::USession> session,
		Core::SHPTR<UNicknameRegistry> nicknameRegistry, Core::SHPTR<UDatabaseManager> databaseManager,
		Core::FString remoteIp);
	virtual ~UAuthHandler() override;
	NO_COPY(UAuthHandler)

	// 연결 종료 시 진행 중인 인증 대기 상태를 해제한다(이 Session의 strand 전용).
	void OnSessionDisconnected() override;

protected:
	// 미인증 상태에서 CS_REGISTER · CS_LOGIN만 허용하고, 나머지 도메인 패킷은 즉시 차단한다.
	void OnDomainPacket(const Core::FPacketView& packet) override;

private:
	void OnPacket(const multiplayer::protocol::CSRegister& packet);
	void OnPacket(const multiplayer::protocol::CSLogin& packet);

	// DB 전용 스레드에서 완료된 회원가입/로그인 결과를 이 Session의 strand에서 반영한다.
	void OnDbRegisterCompleted(EDbRegisterStatus status);
	void OnDbLoginCompleted(const FDbLoginOutcome& outcome);

	// 닉네임 예약 + UPlayer 생성 + 기본 방 비동기 입장 요청 공통 경로다.
	void BeginRoomEnter(Core::FString nickname, Core::uint64 accountId, Core::uint64 connectionLogId);

	// 기본 방 입장 확정 시 USession 핸들러를 UPlayer로 원자 교체하고 로그인 성공 응답을 보낸다.
	void OnEnterRoomCompleted(const Core::SHPTR<UGameRoom>& gameRoom, const Core::SHPTR<UPlayer>& player,
		UGameRoom::EEnterResult result, Core::uint32 userCount);

	Core::SHPTR<UNicknameRegistry> _NicknameRegistry;
	Core::SHPTR<UDatabaseManager> _DatabaseManager;
	Core::FString _RemoteIp;
	bool _LoginPending{ false };    // 연결된 USession의 strand에서만 접근한다.
	bool _RegisterPending{ false }; // 연결된 USession의 strand에서만 접근한다.
	bool _Disconnected{ false };    // 연결된 USession의 strand에서만 접근한다.
};

SPACE_T
