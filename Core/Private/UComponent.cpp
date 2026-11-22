#include "CoreDefine.h"
#include "UComponent.h"
#include "UActor.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UComponent.cpp"

SPACE_H(Core)

UComponent::UComponent(SHPTR<UCoreInstance> coreInstance)
	: UObject{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, UComponent, UComponent, GetRuntimeTraceID());
}

UComponent::~UComponent()
{
	RUNTIME_TRACE_EVENT(Destroy, UComponent, ~UComponent, GetRuntimeTraceID());
}

// 소유자 UActor를 약참조로 연결한다(이미 소유자가 살아 있으면 거절).
bool UComponent::AttachToOwner(const SHPTR<UActor>& owner)
{
	RUNTIME_TRACE_SCOPE(UComponent, AttachToOwner, GetRuntimeTraceID());
	if (!owner || !_Owner.expired())
		return false;

	_Owner = owner;
	return true;
}

SPACE_T
