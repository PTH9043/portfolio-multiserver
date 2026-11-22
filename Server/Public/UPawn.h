#pragma once

#include "CoreDefine.h"
#include "UActor.h"
#include "UComponent.h"

#include <concepts>
#include <memory>

SPACE_H(Server)

// 컴포넌트를 조립할 수 있는 서버 도메인 엔티티 기반 클래스다.
// 컴포넌트는 Append-Only CONUNORMAP으로 보관해 런타임 중 잠금 없이 O(1) 조회한다.
class UPawn abstract : public Core::UActor {
	using COMPONENT_CONTAINER = Core::CONUNORMAP<Core::uint32, Core::SHPTR<UComponent>>;

protected:
	explicit UPawn(Core::SHPTR<Core::UCoreInstance> coreInstance);
	virtual ~UPawn() override;

public:
	// 컴포넌트를 컴파일 시점 타입 검사 후 부착한다.
	template<class TComponent>
		requires std::derived_from<TComponent, UComponent>
	bool AddComponent(const Core::SHPTR<TComponent>& component)
	{
		static_assert(TComponent::ComponentTypeID != static_cast<Core::uint32>(EComponentType::None),
			"ComponentTypeID must not be EComponentType::None");
		if (!component || component->GetComponentTypeID() != TComponent::ComponentTypeID)
			return false;
		return AddComponentInternal(std::static_pointer_cast<UComponent>(component));
	}

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 해당 유형 ID의 컴포넌트가 없으면 빈 포인터다.
	Core::SHPTR<UComponent> FindComponent(Core::uint32 typeID) const;

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. TypeID 일치 확인 후 static_pointer_cast로 돌려준다.
	template<class TComponent>
		requires std::derived_from<TComponent, UComponent>
	Core::SHPTR<TComponent> GetComponent() const
	{
		const auto component = FindComponent(TComponent::ComponentTypeID);
		if (!component || component->GetComponentTypeID() != TComponent::ComponentTypeID)
			return nullptr;
		return std::static_pointer_cast<TComponent>(component);
	}

private:
	NO_COPY(UPawn)

	// 타입 검사를 통과한 컴포넌트를 내부 컨테이너에 등록하고 소유자(_Owner)를 연결한다.
	bool AddComponentInternal(const Core::SHPTR<UComponent>& component);

	COMPONENT_CONTAINER _ComponentContainer;
};

SPACE_T
