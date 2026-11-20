#pragma once

#include "USession.h"
#include "UGameRoom.h"

namespace multiplayer::protocol {
class CSLogin;
class CSChat;
class CSRoomUsers;
}

SPACE_H(Server)

class UPlayer;

// 클라이언트 연결 하나다. 로그인한 UPlayer를 소유(SHPTR)하며 방 처리는 UGameRoom으로 연결한다.
// 패킷 본문은 Protocol Buffers이며 Core는 그 형식을 모른다.
class UServerSession final : public Core::USession {
public:
	explicit UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket);
	virtual ~UServerSession() override;

protected:
	// 머리말의 패킷 번호(ECSPacket)로 본문의 메시지 타입을 골라 HandlePacket에 넘긴다.
	// 서버가 받지 않는 번호(CS_NONE 포함)는 RejectPacket으로 연결을 닫는다.
	void ProcessingData(Core::uint8* packetData, Core::uint64 packetSize) override;

	// 이 Session의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다.
	// 로그인한 UPlayer가 방에 속해 있으면 퇴장을 요청하고 소유를 해제한다.
	void OnDisconnected() override;

private:
	// 모든 패킷이 함께 쓰는 수신 처리 함수다. body를 TPacket으로 해석하고, 실패하면 RejectPacket으로 연결을 닫는다.
	// 해석에 성공하면 패킷별 OnPacket 오버로드에 넘긴다.
	// ProcessingData와 같은 이 Session의 strand에서 호출되며 body는 반환할 때까지만 유효하다.
	template<class TPacket>
	void HandlePacket(Core::uint16 packetType, const Core::uint8* body, Core::uint64 bodySize);

	// 모든 서버→클라이언트 패킷이 함께 쓰는 송신 함수다.
	// 직렬화 크기가 PACKET_MAX_BODY_SIZE를 넘으면 Warning을 기록하고 보내지 않는다.
	template<class TPacket>
	void SendPacket(Core::uint16 packetType, const TPacket& packet);

	// 이 Session의 strand에서 실행되는 패킷별 처리 함수다.
	void OnPacket(const multiplayer::protocol::CSLogin& packet);
	void OnPacket(const multiplayer::protocol::CSChat& packet);
	void OnPacket(const multiplayer::protocol::CSRoomUsers& packet);

	// UGameRoom의 비동기 입장 판정 결과를 이 Session의 strand에서 반영한다.
	// 입장 진행 도중 연결이 이미 끊겼다면 방에 퇴장을 요청해 유령 플레이어를 방지한다.
	void OnEnterRoomCompleted(const Core::SHPTR<UGameRoom>& gameRoom, const Core::SHPTR<UPlayer>& player,
		UGameRoom::EEnterResult result, Core::uint32 userCount);

	// 프로토콜을 지키지 않는 클라이언트의 연결은 유지하지 않는다. 이유를 Warning으로 기록하고 연결을 닫는다.
	void RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason);

	Core::SHPTR<UPlayer> _Player;       // 이 Session의 strand에서만 접근한다.
	Core::SHPTR<UGameRoom> _GameRoom;   // 이 Session의 strand에서만 접근한다.
	bool _LoginPending{ false };        // 이 Session의 strand에서만 접근한다.
	bool _Disconnected{ false };        // 이 Session의 strand에서만 접근한다.
};

SPACE_T
