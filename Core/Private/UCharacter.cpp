#include "CoreDefine.h"
#include "UCharacter.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UCharacter.cpp"

SPACE_H(Core)

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
