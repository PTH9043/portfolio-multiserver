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

// 기본 액터는 네트워크 세션을 가지지 않으며, UPawn에서 세션 컴포넌트 조회로 재정의한다.
SHPTR<USession> UActor::GetSession() const noexcept
{
	return nullptr;
}

SPACE_T
