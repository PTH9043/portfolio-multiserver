#pragma once

#include "UActor.h"
#include "UComponent.h"

#include <concepts>
#include <memory>

SPACE_H(Core)

class FPacketView;
class USession;

// 화면에 그려지지만 스스로 움직일 수 없는 사물의 기반 클래스다.
// 컴포넌트는 Append-Only CONUNORMAP으로 보관해 런타임 중 잠금 없이 O(1) 조회한다.
class CORE_DLL UPawn abstract : public UActor {
	using COMPONENT_CONTAINER = CONUNORMAP<uint32, SHPTR<UComponent>>;

protected:
	explicit UPawn(SHPTR<UCoreInstance> coreInstance);
	virtual ~UPawn() override;

public:
	// 컴포넌트를 컴파일 시점 타입 검사 후 부착한다(초기화 시점·컴포넌트 소유 스레드 전용).
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

	// 부착된 USession 컴포넌트를 잠금 없이 O(1)로 돌려준다. 부착되지 않았으면 빈 포인터다.
	SHPTR<USession> GetSession() const noexcept override;

	// 부착된 USession의 strand에서 완성된 패킷 뷰를 전달받을 때 호출된다.
	// packet은 동기 스코프 전용(NO_COPY)이며, 다른 스레드·strand로 넘길 때는 packet.Detach()로 불변 페이로드를 분리한다.
	virtual void OnReceivePacket(const FPacketView& packet);

	// 부착된 USession의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다.
	virtual void OnSessionDisconnected();

private:
	NO_COPY(UPawn)

	// 타입 검사를 통과한 컴포넌트를 내부 컨테이너에 등록하고 소유자(_Owner)를 연결한다.
	bool AddComponentInternal(const SHPTR<UComponent>& component);

	COMPONENT_CONTAINER _ComponentContainer;
};

SPACE_T
