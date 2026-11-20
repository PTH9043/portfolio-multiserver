#include "UPlayer.h"
#include "USession.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UPlayer.cpp"

SPACE_H(Server)

UPlayer::UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance, FPlayerInfo info, Core::WKPTR<Core::USession> session)
	: UCharacter{ std::move(coreInstance) },
	  _Info{ std::move(info) },
	  _Session{ std::move(session) }
{
	RUNTIME_TRACE_EVENT(Create, UPlayer, UPlayer, GetRuntimeTraceID());
}

UPlayer::~UPlayer()
{
	RUNTIME_TRACE_EVENT(Destroy, UPlayer, ~UPlayer, GetRuntimeTraceID());
}

Core::SHPTR<Core::USession> UPlayer::GetSession() const noexcept
{
	return _Session.lock();
}

SPACE_T
