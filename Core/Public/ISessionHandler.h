#pragma once

#include "CoreDefine.h"

SPACE_H(Core)

class FPacketView;

// USession에서 수신한 완성 패킷과 연결 종료 통지를 위임받는 추상 핸들러 인터페이스다.
// 모든 콜백은 해당 USession의 strand에서 직렬화되어 호출된다.
class CORE_DLL ISessionHandler abstract {
public:
	virtual ~ISessionHandler() = default;

	// USession의 strand에서 완성된 패킷 뷰를 전달받을 때 호출된다.
	// packet은 동기 스코프 전용(NO_COPY)이며, 다른 스레드·strand로 넘길 때는 packet.Detach()로 불변 복사한다.
	virtual void OnReceivePacket(const FPacketView& packet) = 0;

	// USession의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다.
	virtual void OnSessionDisconnected() = 0;
};

SPACE_T
