#include "CoreDefine.h"
#include "UPawn.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UPawn.cpp"

SPACE_H(Core)

UPawn::UPawn(SHPTR<UCoreInstance> coreInstance)
	: UActor{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, UPawn, UPawn, GetRuntimeTraceID());
}

UPawn::~UPawn()
{
	RUNTIME_TRACE_EVENT(Destroy, UPawn, ~UPawn, GetRuntimeTraceID());
}

SPACE_T
