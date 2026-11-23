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

// 유형 ID로 등록된 컴포넌트를 잠금 없이 O(1) 조회한다.
SHPTR<UComponent> UActor::FindComponent(const uint32 typeID) const
{
	if (typeID == static_cast<uint32>(EComponentType::None))
		return nullptr;

	const auto it = _ComponentContainer.find(typeID);
	if (it == _ComponentContainer.end())
		return nullptr;
	return it->second;
}

// 장착된 USession 컴포넌트를 캐시된 원자 포인터로 해시 탐색·RTTI 비용 없이 O(1)로 바로 꺼낸다.
SHPTR<USession> UActor::GetSession() const noexcept
{
	USession* const cachedSession{ _CachedSession.load(std::memory_order_acquire) };
	if (cachedSession == nullptr)
		return nullptr;
	// Append-Only 컨테이너가 USession 수명을 보장하므로 weak_from_this().lock()으로 안전하게 승격한다.
	return std::static_pointer_cast<USession>(cachedSession->weak_from_this().lock());
}

// 기본 구현은 수신 패킷을 무시하며, 패킷을 처리하는 파생 클래스(Server::UPawn 등)에서 재정의한다.
void UActor::OnReceivePacket(const FPacketView& packet)
{
	(void)packet;
}

// 기본 구현은 빈 훅이며, 연결 종료 후처리(방 퇴장 등)가 필요한 파생 클래스에서 재정의한다.
void UActor::OnSessionDisconnected()
{
}

// 타입 검사를 통과한 컴포넌트를 유형 ID 슬롯에 등록하고 소유자(_Owner)를 연결한다.
bool UActor::AddComponentInternal(const SHPTR<UComponent>& component)
{
	RUNTIME_TRACE_SCOPE(UActor, AddComponentInternal, GetRuntimeTraceID());
	if (!component)
		return false;

	const uint32 typeID{ component->GetComponentTypeID() };
	if (typeID == static_cast<uint32>(EComponentType::None) || component->HasOwner())
		return false;

	const auto self = ThisWeak<UActor>().lock();
	if (!self)
		return false;

	// 이미 같은 유형이 등록돼 있으면 삽입·소유자 변경 전에 빠르게 거절한다.
	if (_ComponentContainer.find(typeID) != _ComponentContainer.end())
		return false;

	const auto [it, inserted] = _ComponentContainer.insert({ typeID, component });
	(void)it;
	if (!inserted)
		return false;

	if (!component->AttachToOwner(self))
		return false;

	if (typeID == USession::ComponentTypeID)
		_CachedSession.store(static_cast<USession*>(component.get()), std::memory_order_release);
	return true;
}

SPACE_T
