#include "UServerSession.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerSession.cpp"

SPACE_H(Server)

UServerSession::UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket,
	Core::FString remoteIp)
	: USession{ std::move(coreInstance), std::move(socket) },
	  _RemoteIp{ std::move(remoteIp) }
{
	RUNTIME_TRACE_EVENT(Create, UServerSession, UServerSession, GetRuntimeTraceID());
}

UServerSession::~UServerSession()
{
	RUNTIME_TRACE_EVENT(Destroy, UServerSession, ~UServerSession, GetRuntimeTraceID());
}

SPACE_T
