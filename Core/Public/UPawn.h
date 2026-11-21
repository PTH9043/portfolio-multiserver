#pragma once

#include "UActor.h"
#include "UComponent.h"

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
	// 컴포넌트를 이 UPawn에 부착하고 소유자(_Owner)를 연결한다(초기화 시점·컴포넌트 소유 스레드 전용).
	// component가 비어 있거나 유형 ID가 0이거나, Session 슬롯에 USession이 아닌 객체를 넣거나, 이미 부착·중복된 경우 false다.
	// shared_ptr 소유 상태에서 호출해야 한다.
	bool AddComponent(const SHPTR<UComponent>& component);

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 해당 유형 ID의 컴포넌트가 없으면 빈 포인터다.
	SHPTR<UComponent> FindComponent(uint32 typeID) const;

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. TComponent::ComponentTypeID로 조회해 돌려준다.
	template<class TComponent>
	SHPTR<TComponent> GetComponent() const
	{
		return std::dynamic_pointer_cast<TComponent>(FindComponent(TComponent::ComponentTypeID));
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

	COMPONENT_CONTAINER _ComponentContainer;
};

SPACE_T
