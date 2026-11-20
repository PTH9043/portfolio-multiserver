#include "CoreDefine.h"
#include "UActor.h"
#include "USession.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UActor.cpp"

SPACE_H(Core)

UActor::UActor(SHPTR<UCoreInstance> coreInstance)
	: UObject{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, UActor, UActor, GetRuntimeTraceID());
}

UActor::~UActor()
{
	RUNTIME_TRACE_EVENT(Destroy, UActor, ~UActor, GetRuntimeTraceID());
}

SHPTR<USession> UActor::GetSession() const noexcept
{
	return nullptr;
}

SPACE_T
