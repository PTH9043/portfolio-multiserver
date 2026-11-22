#include "UPawn.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UPawn.cpp"

SPACE_H(Server)

UPawn::UPawn(Core::SHPTR<Core::UCoreInstance> coreInstance)
	: UActor{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, UPawn, UPawn, GetRuntimeTraceID());
}

UPawn::~UPawn()
{
	RUNTIME_TRACE_EVENT(Destroy, UPawn, ~UPawn, GetRuntimeTraceID());
}

// 타입 검사를 통과한 컴포넌트를 유형 ID 슬롯에 등록하고 소유자(_Owner)를 연결한다.
bool UPawn::AddComponentInternal(const Core::SHPTR<UComponent>& component)
{
	RUNTIME_TRACE_SCOPE(UPawn, AddComponentInternal, GetRuntimeTraceID());
	if (!component)
		return false;

	const Core::uint32 typeID{ component->GetComponentTypeID() };
	if (typeID == static_cast<Core::uint32>(EComponentType::None) || component->HasOwner())
		return false;

	const auto self = ThisWeak<UPawn>().lock();
	if (!self)
		return false;

	// 이미 같은 유형이 등록돼 있으면 삽입·소유자 변경 전에 빠르게 거절한다.
	if (_ComponentContainer.find(typeID) != _ComponentContainer.end())
		return false;

	const auto [it, inserted] = _ComponentContainer.insert({ typeID, component });
	(void)it;
	if (!inserted)
		return false;

	return component->AttachToOwner(self);
}

// 유형 ID로 등록된 컴포넌트를 잠금 없이 O(1) 조회한다.
Core::SHPTR<UComponent> UPawn::FindComponent(const Core::uint32 typeID) const
{
	if (typeID == static_cast<Core::uint32>(EComponentType::None))
		return nullptr;

	const auto it = _ComponentContainer.find(typeID);
	if (it == _ComponentContainer.end())
		return nullptr;
	return it->second;
}

SPACE_T
