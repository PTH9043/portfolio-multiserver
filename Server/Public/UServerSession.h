#pragma once

#include "USession.h"

SPACE_H(Server)

// 클라이언트 연결 하나다. 플레이어와 몬스터 AI 모두 이 세션으로 접속한다.
// 로그인 패킷을 받으면 액터와 연결할 예정이다. 패킷 본문은 Protocol Buffers이며 Core는 그 형식을 모른다.
class UServerSession final : public Core::USession {
public:
	explicit UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket);
	virtual ~UServerSession() override;

protected:
	// 머리말의 패킷 번호(ECSPacket)로 본문의 메시지 타입을 골라 HandlePacket에 넘긴다.
	// 서버가 받지 않는 번호(CS_NONE 포함)는 RejectPacket으로 연결을 닫는다.
	void ProcessingData(Core::uint8* packetData, Core::uint64 packetSize) override;

private:
	// 모든 패킷이 함께 쓰는 처리 함수다. body를 TPacket으로 해석하고, 실패하면 RejectPacket으로 연결을 닫는다.
	// 지금은 해석한 내용을 Info로 기록만 한다. 로그인·채팅 처리를 붙일 때 패킷별로 나눌지 정한다.
	// ProcessingData와 같은 이 Session의 strand에서 호출되며 body는 반환할 때까지만 유효하다.
	template<class TPacket>
	void HandlePacket(Core::uint16 packetType, const Core::uint8* body, Core::uint64 bodySize);

	// 프로토콜을 지키지 않는 클라이언트의 연결은 유지하지 않는다. 이유를 Warning으로 기록하고 연결을 닫는다.
	void RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason);
};

SPACE_T
