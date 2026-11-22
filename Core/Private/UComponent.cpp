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

// 소유자 UActor를 약참조로 연결하고 장착 완료 훅을 호출한다(이미 소유자가 살아 있으면 거절).
bool UComponent::AttachToOwner(const SHPTR<UActor>& owner)
{
	RUNTIME_TRACE_SCOPE(UComponent, AttachToOwner, GetRuntimeTraceID());
	if (!owner || !_Owner.expired())
		return false;

	_Owner = owner;
	OnAttached(owner);
	return true;
}

// 기본 구현은 빈 훅이며, 장착 직후 초기화가 필요한 파생 컴포넌트에서 재정의한다.
void UComponent::OnAttached(const SHPTR<UActor>& owner)
{
	(void)owner;
}

SPACE_T
