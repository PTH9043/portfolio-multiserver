#pragma once

#include "USession.h"
#include "UGameRoom.h"

namespace multiplayer::protocol {
class CSLogin;
class CSPing;
}

SPACE_H(Server)

class UPlayer;
class UNicknameRegistry;

// 클라이언트 연결 하나다. 로그인 전 핸드셰이크(CS_LOGIN·CS_PING)만 받아 UPlayer를 생성·부착한다.
// 부착 완료 이후의 도메인 패킷 가공과 퇴장 후처리는 소유자 UPawn(UPlayer)이 직접 담당한다.
class UServerSession final : public Core::USession {
public:
	explicit UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket,
		Core::SHPTR<UNicknameRegistry> nicknameRegistry = nullptr);
	virtual ~UServerSession() override;

protected:
	// 소유자 UPawn이 아직 부착되지 않은 로그인 전 패킷만 처리한다.
	// 로그인 전 CS_CHAT·CS_ROOM_USERS·CS_ROOM_LIST·CS_JOIN_ROOM이나 미정의 번호는 RejectPacket으로 연결을 닫는다.
	void OnUnboundPacket(const Core::FPacketView& packet) override;

	// 이 Session의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다.
	void OnDisconnected() override;

private:
	// 로그인 전 수신 패킷을 TPacket으로 해석하고, 실패하면 RejectPacket으로 연결을 닫는다.
	template<class TPacket>
	void HandlePacket(const Core::FPacketView& packet);

	// 로그인 응답 등 서버→클라이언트 패킷을 직렬화·전송한다.
	template<class TPacket>
	void SendPacket(Core::uint16 packetType, const TPacket& packet);

	// 이 Session의 strand에서 실행되는 로그인 전 패킷 처리 함수다.
	void OnPacket(const multiplayer::protocol::CSLogin& packet);
	void OnPacket(const multiplayer::protocol::CSPing& packet);

	// UGameRoom의 비동기 입장 판정 결과를 이 Session의 strand에서 반영한다.
	// 입장 진행 도중 연결이 이미 끊겼다면 방에 퇴장을 요청해 유령 플레이어를 방지한다.
	void OnEnterRoomCompleted(const Core::SHPTR<UGameRoom>& gameRoom, const Core::SHPTR<UPlayer>& player,
		UGameRoom::EEnterResult result, Core::uint32 userCount);

	// 프로토콜을 지키지 않는 클라이언트의 연결은 유지하지 않는다. 이유를 Warning으로 기록하고 연결을 닫는다.
	void RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason);

	Core::SHPTR<UNicknameRegistry> _NicknameRegistry;
	bool _LoginPending{ false };        // 이 Session의 strand에서만 접근한다.
	bool _Disconnected{ false };        // 이 Session의 strand에서만 접근한다.
};

SPACE_T
