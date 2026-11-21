#include "UServerListener.h"
#include "UServerSession.h"
#include "UCoreInstance.h"
#include "UNetworkManager.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerListener.cpp"

SPACE_H(Server)

UServerListener::UServerListener(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context,
	Core::SHPTR<UNicknameRegistry> nicknameRegistry, const Core::MILLISECONDS sessionIdleTimeout)
	: UListener{ std::move(coreInstance), context },
	  _NicknameRegistry{ std::move(nicknameRegistry) },
	  _SessionIdleTimeout{ sessionIdleTimeout }
{
	RUNTIME_TRACE_EVENT(Create, UServerListener, UServerListener, GetRuntimeTraceID());
}

UServerListener::~UServerListener()
{
	RUNTIME_TRACE_EVENT(Destroy, UServerListener, ~UServerListener, GetRuntimeTraceID());
}

// 새 TCP 연결을 UServerSession으로 감싸 유휴 타임아웃을 적용하고 네트워크 관리자에 등록한 뒤 수신을 시작한다.
void UServerListener::HandleAcceptedSocket(Core::TCPSOCKET socket)
{
	RUNTIME_TRACE_SCOPE(UServerListener, HandleAcceptedSocket, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto networkManager = coreInstance ? coreInstance->GetNetworkManager() : nullptr;
	if (!networkManager)
		return;

	const auto session = Core::MakeShared<UServerSession>(coreInstance, std::move(socket), _NicknameRegistry);
	session->SetIdleTimeout(_SessionIdleTimeout);
	// 등록을 먼저 게시해야 수신이 바로 끝나 Close가 게시하는 목록 제거가 등록 뒤에 실행된다.
	networkManager->RequestAddSession(session);
	session->Receive();
}

SPACE_T
