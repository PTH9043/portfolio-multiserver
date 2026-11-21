#include "CoreDefine.h"
#include "UPawn.h"
#include "Packet.h"
#include "USession.h"

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

// 컴포넌트를 유형 ID 슬롯에 등록하고 소유자(_Owner)를 연결한다.
bool UPawn::AddComponent(const SHPTR<UComponent>& component)
{
	RUNTIME_TRACE_SCOPE(UPawn, AddComponent, GetRuntimeTraceID());
	if (!component)
		return false;

	const uint32 typeID{ component->GetComponentTypeID() };
	if (typeID == static_cast<uint32>(EComponentType::None) || component->HasOwner())
		return false;

	// GetSession()의 static_pointer_cast 안전성을 보장하기 위해 Session 슬롯은 USession 파생형만 허용한다.
	if (typeID == USession::ComponentTypeID && !std::dynamic_pointer_cast<USession>(component))
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
SHPTR<UComponent> UPawn::FindComponent(const uint32 typeID) const
{
	if (typeID == static_cast<uint32>(EComponentType::None))
		return nullptr;

	const auto it = _ComponentContainer.find(typeID);
	if (it == _ComponentContainer.end())
		return nullptr;
	return it->second;
}

// 부착된 USession 컴포넌트를 RTTI 비용 없이 바로 꺼낸다.
SHPTR<USession> UPawn::GetSession() const noexcept
{
	const auto it = _ComponentContainer.find(USession::ComponentTypeID);
	if (it == _ComponentContainer.end())
		return nullptr;
	// AddComponent에서 USession 파생형만 등록하도록 보장하므로 정적 캐스팅이 안전하다.
	return std::static_pointer_cast<USession>(it->second);
}

// 기본 구현은 수신 패킷을 무시하며, 도메인 패킷을 처리하는 파생 클래스(UPlayer 등)에서 재정의한다.
void UPawn::OnReceivePacket(const FPacketView& packet)
{
	(void)packet;
}

// 기본 구현은 빈 훅이며, 연결 종료 후처리(방 퇴장 등)가 필요한 파생 클래스에서 재정의한다.
void UPawn::OnSessionDisconnected()
{
}

SPACE_T
