#pragma once

#include "UObject.h"
#include "UComponent.h"

#include <concepts>
#include <memory>

SPACE_H(Core)

class FPacketView;
class USession;

// 액터 종류를 식별하는 고정 키다. 상위 프로젝트(Server 등)는 UserStart 이후 값을 할당해 RTTI 없이 판별한다.
enum class EActorType : uint32 {
	None = 0,
	UserStart = 1000
};

// 세상에 존재하는 사물의 기반 클래스다.
// 컴포넌트(USession 등)를 Append-Only CONUNORMAP으로 장착·보유해 런타임 중 잠금 없이 O(1) 조회한다.
class CORE_DLL UActor abstract : public UObject {
	using COMPONENT_CONTAINER = CONUNORMAP<uint32, SHPTR<UComponent>>;

protected:
	explicit UActor(SHPTR<UCoreInstance> coreInstance);
	virtual ~UActor() override;

public:
	// 액터의 고정 유형 ID를 돌려준다. 파생 액터는 정적 ActorTypeID 상수와 일치하는 값을 반환해 static_cast 안전성을 보장한다.
	virtual uint32 GetActorTypeID() const noexcept { return static_cast<uint32>(EActorType::None); }

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 0이면 현재 어떤 방에도 속하지 않은 상태다.
	uint64 GetRoomID() const noexcept { return _RoomID.load(std::memory_order_acquire); }

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 방 입·퇴장 시 URoom이 소속 방 ID를 갱신한다.
	void SetRoomID(uint64 roomID) noexcept { _RoomID.store(roomID, std::memory_order_release); }

	// 현재 소속 방 ID가 expectedRoomID와 일치할 때만 0으로 초기화한다.
	// 방 이동 직후 이전 방의 늦은 퇴장 처리가 새 방 ID를 덮어쓰는 경합을 막는다.
	bool ClearRoomID(uint64 expectedRoomID) noexcept
	{
		if (expectedRoomID == 0)
			return false;
		return _RoomID.compare_exchange_strong(expectedRoomID, 0, std::memory_order_acq_rel, std::memory_order_acquire);
	}

	// 컴포넌트를 컴파일 시점 타입 검사 후 장착한다(초기화 시점 전용).
	// Session 슬롯은 USession 파생형만 컴파일을 허용해 GetSession/GetComponent의 static_pointer_cast 안전성을 보장한다.
	template<class TComponent>
		requires std::derived_from<TComponent, UComponent>
	bool AddComponent(const SHPTR<TComponent>& component)
	{
		static_assert(TComponent::ComponentTypeID != static_cast<uint32>(EComponentType::None),
			"ComponentTypeID must not be EComponentType::None");
		if constexpr (TComponent::ComponentTypeID == static_cast<uint32>(EComponentType::Session)) {
			static_assert(std::derived_from<TComponent, USession>,
				"EComponentType::Session slot requires a USession-derived type");
		}
		if (!component || component->GetComponentTypeID() != TComponent::ComponentTypeID)
			return false;
		return AddComponentInternal(std::static_pointer_cast<UComponent>(component));
	}

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 해당 유형 ID의 컴포넌트가 없으면 빈 포인터다.
	SHPTR<UComponent> FindComponent(uint32 typeID) const;

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 컴파일 시점 등록 규약과 TypeID 일치 확인 후 static_pointer_cast로 돌려준다.
	template<class TComponent>
		requires std::derived_from<TComponent, UComponent>
	SHPTR<TComponent> GetComponent() const
	{
		const auto component = FindComponent(TComponent::ComponentTypeID);
		if (!component || component->GetComponentTypeID() != TComponent::ComponentTypeID)
			return nullptr;
		return std::static_pointer_cast<TComponent>(component);
	}

	// 장착된 USession 컴포넌트를 캐시된 원자 포인터로 해시 탐색 없이 잠금 없이 O(1) 돌려준다. 장착되지 않았으면 빈 포인터다.
	virtual SHPTR<USession> GetSession() const noexcept;

	// 장착된 USession의 strand에서 완성된 SafeGuard 패킷 뷰(NO_COPY)를 전달받을 때 호출된다.
	// 다른 스레드·strand로 넘길 때는 packet.Detach()로 불변 페이로드를 분리한다.
	virtual void OnReceivePacket(const FPacketView& packet);

	// 장착된 USession의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다.
	virtual void OnSessionDisconnected();

private:
	NO_COPY(UActor)

	// 타입 검사를 통과한 컴포넌트를 내부 컨테이너에 등록하고 소유자(_Owner)를 연결한다.
	bool AddComponentInternal(const SHPTR<UComponent>& component);

	ATOMIC_UINT64 _RoomID{ 0 };
	ATOMIC<USession*> _CachedSession{ nullptr };
	COMPONENT_CONTAINER _ComponentContainer;
};

SPACE_T
