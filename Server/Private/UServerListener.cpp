#include "UServerListener.h"
#include "UServerSession.h"
#include "UCoreInstance.h"
#include "UNetworkManager.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerListener.cpp"

SPACE_H(Server)

UServerListener::UServerListener(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context)
	: UListener{ std::move(coreInstance), context }
{
	RUNTIME_TRACE_EVENT(Create, UServerListener, UServerListener, GetRuntimeTraceID());
}

UServerListener::~UServerListener()
{
	RUNTIME_TRACE_EVENT(Destroy, UServerListener, ~UServerListener, GetRuntimeTraceID());
}

void UServerListener::HandleAcceptedSocket(Core::TCPSOCKET socket)
{
	RUNTIME_TRACE_SCOPE(UServerListener, HandleAcceptedSocket, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto networkManager = coreInstance ? coreInstance->GetNetworkManager() : nullptr;
	if (!networkManager)
		return;

	const auto session = Core::MakeShared<UServerSession>(coreInstance, std::move(socket));
	// 등록을 먼저 게시해야 수신이 바로 끝나 Close가 게시하는 목록 제거가 등록 뒤에 실행된다.
	networkManager->RequestAddSession(session);
	session->Receive();
}

SPACE_T
