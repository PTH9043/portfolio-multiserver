#include "UComponent.h"
#include "UPawn.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UComponent.cpp"

SPACE_H(Server)

UComponent::UComponent(Core::SHPTR<Core::UCoreInstance> coreInstance)
	: UObject{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, UComponent, UComponent, GetRuntimeTraceID());
}

UComponent::~UComponent()
{
	RUNTIME_TRACE_EVENT(Destroy, UComponent, ~UComponent, GetRuntimeTraceID());
}

// 소유자 UPawn을 약참조로 연결하고 부착 완료 훅을 호출한다(이미 소유자가 살아 있으면 거절).
bool UComponent::AttachToOwner(const Core::SHPTR<UPawn>& owner)
{
	RUNTIME_TRACE_SCOPE(UComponent, AttachToOwner, GetRuntimeTraceID());
	if (!owner || !_Owner.expired())
		return false;

	_Owner = owner;
	OnAttached(owner);
	return true;
}

// 기본 구현은 빈 훅이며, 부착 직후 초기화가 필요한 파생 컴포넌트에서 재정의한다.
void UComponent::OnAttached(const Core::SHPTR<UPawn>& owner)
{
	(void)owner;
}

SPACE_T
