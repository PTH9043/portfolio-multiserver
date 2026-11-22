#pragma once

#include "CoreDefine.h"
#include "USession.h"

SPACE_H(Server)

// 서버 전용 전송 세션 컴포넌트다(Core::USession 확장 지점).
// 도메인 패킷(로그인·채팅·DB)을 직접 해석하지 않으며, 완성된 SafeGuard(FPacketView)는
// Core::USession을 통해 장착된 소유자 UActor(UPawn/UPlayer)로 그대로 전달한다.
class UServerSession final : public Core::USession {
public:
	STATIC_CONSTEXPR Core::uint32 SessionTypeID{ 1 };

	explicit UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket,
		Core::FString remoteIp = Core::FString{ "127.0.0.1" });
	virtual ~UServerSession() override;
	NO_COPY(UServerSession)

	// RTTI 없이 UServerSession을 판별하기 위한 고정 세션 유형 ID를 돌려준다.
	Core::uint32 GetSessionTypeID() const noexcept override { return SessionTypeID; }

	// 연결 수락 시 확정된 불변 원격 IP 참조다. 이 세션이 살아 있는 동안 유효하다.
	const Core::FString& GetRemoteIp() const noexcept { return _RemoteIp; }

private:
	Core::FString _RemoteIp;
};

SPACE_T
