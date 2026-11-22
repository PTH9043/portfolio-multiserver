#pragma once

#include "UListener.h"
#include "USession.h"

SPACE_H(Server)

class UPlayer;
class UNicknameRegistry;
class UDatabaseManager;

// 수락한 소켓으로 UServerSession(UComponent)과 UPlayer(UPawn)를 생성·장착하고 수신을 시작한다.
// 접속 중인 UPlayer의 수명(특히 로그인 전 방 입장 전까지)을 비순환 소유권으로 유지하며 연결 종료 시 즉시 해제한다.
class UServerListener final : public Core::UListener {
public:
	// context는 UNetworkManager의 io_context여야 한다.
	explicit UServerListener(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context,
		Core::SHPTR<UNicknameRegistry> nicknameRegistry = nullptr,
		Core::MILLISECONDS sessionIdleTimeout = Core::SESSION_DEFAULT_IDLE_TIMEOUT,
		Core::SHPTR<UDatabaseManager> databaseManager = nullptr);
	virtual ~UServerListener() override;

protected:
	// 네트워크 워커에서 호출된다. 플레이어·세션 생성, 컴포넌트 장착, 등록 게시, 수신 시작을 수행한다.
	void HandleAcceptedSocket(Core::TCPSOCKET socket) override;

private:
	// 연결이 종료된 플레이어를 활성 목록에서 제거한다(어느 워커 스레드에서나 호출 가능).
	void UnregisterConnectedPlayer(const Core::SHPTR<UPlayer>& player) noexcept;

	Core::SHPTR<UNicknameRegistry> _NicknameRegistry;
	Core::SHPTR<UDatabaseManager> _DatabaseManager;
	Core::MILLISECONDS _SessionIdleTimeout{ Core::SESSION_DEFAULT_IDLE_TIMEOUT };
	Core::SHARED_MUTEX _PlayerMutex;
	Core::UNORSET<Core::SHPTR<UPlayer>> _ConnectedPlayerContainer;
};

SPACE_T
