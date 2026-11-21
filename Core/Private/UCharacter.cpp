#include "CoreDefine.h"
#include "UCharacter.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UCharacter.cpp"

SPACE_H(Core)

// UPawn을 상속하는 이동 가능 캐릭터 기반 클래스의 생성·소멸을 기록한다.
UCharacter::UCharacter(SHPTR<UCoreInstance> coreInstance)
	: UPawn{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, UCharacter, UCharacter, GetRuntimeTraceID());
}

UCharacter::~UCharacter()
{
	RUNTIME_TRACE_EVENT(Destroy, UCharacter, ~UCharacter, GetRuntimeTraceID());
}

SPACE_T
