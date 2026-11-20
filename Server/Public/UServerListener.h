#pragma once

#include "UListener.h"

SPACE_H(Server)

// 수락한 소켓으로 UServerSession을 만들고 UNetworkManager에 등록한 뒤 수신을 시작한다.
class UServerListener final : public Core::UListener {
public:
	// context는 UNetworkManager의 io_context여야 한다.
	explicit UServerListener(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context);
	virtual ~UServerListener() override;

protected:
	// 네트워크 워커에서 호출된다. 세션 생성·등록 게시·수신 시작의 예외는 UListener가 받아 리스너를 멈추고
	// 네트워크 워커의 오류 경로로 전달한다. CoreInstance가 없으면 소켓은 닫힌다.
	void HandleAcceptedSocket(Core::TCPSOCKET socket) override;
};

SPACE_T
