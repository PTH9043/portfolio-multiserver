#include "UServerListener.h"
#include "UAuthHandler.h"
#include "UCoreInstance.h"
#include "UNetworkManager.h"
#include "USession.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerListener.cpp"

SPACE_H(Server)

UServerListener::UServerListener(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context,
	Core::SHPTR<UNicknameRegistry> nicknameRegistry, const Core::MILLISECONDS sessionIdleTimeout,
	Core::SHPTR<UDatabaseManager> databaseManager)
	: UListener{ std::move(coreInstance), context },
	  _NicknameRegistry{ std::move(nicknameRegistry) },
	  _DatabaseManager{ std::move(databaseManager) },
	  _SessionIdleTimeout{ sessionIdleTimeout }
{
	RUNTIME_TRACE_EVENT(Create, UServerListener, UServerListener, GetRuntimeTraceID());
}

UServerListener::~UServerListener()
{
	RUNTIME_TRACE_EVENT(Destroy, UServerListener, ~UServerListener, GetRuntimeTraceID());
}

// 새 TCP 연결을 순수 Core::USession으로 감싸고 초기 미인증 핸들러(UAuthHandler)를 바인딩한 뒤 수신을 시작한다.
void UServerListener::HandleAcceptedSocket(Core::TCPSOCKET socket)
{
	RUNTIME_TRACE_SCOPE(UServerListener, HandleAcceptedSocket, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto networkManager = coreInstance ? coreInstance->GetNetworkManager() : nullptr;
	if (!networkManager)
		return;

	Core::FString remoteIp{ "127.0.0.1" };
	{
		Core::BOOST_ERROR_CODE ec;
		const auto endpoint = socket.remote_endpoint(ec);
		if (!ec) {
			try {
				const std::string addr = endpoint.address().to_string();
				remoteIp = Core::FString{ Core::STRINGVIEW{ addr } };
			}
			catch (...) {
			}
		}
	}

	const auto session = Core::MakeShared<Core::USession>(coreInstance, std::move(socket));
	const auto authHandler = Core::MakeShared<UAuthHandler>(
		coreInstance, session, _NicknameRegistry, _DatabaseManager, std::move(remoteIp));
	session->SetHandler(authHandler);
	session->SetIdleTimeout(_SessionIdleTimeout);
	// 등록을 먼저 게시해야 수신이 바로 끝나 Close가 게시하는 목록 제거가 등록 뒤에 실행된다.
	networkManager->RequestAddSession(session);
	session->Receive();
}

SPACE_T
