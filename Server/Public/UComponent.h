#pragma once

#include "CoreDefine.h"
#include "UObject.h"

SPACE_H(Server)

class UPawn;

// 게임 엔티티(UPawn)에 부착하는 컴포넌트 유형 ID다.
enum class EComponentType : Core::uint32 {
	None = 0,
	UserStart = 1000
};

// Server::UPawn에 부착해 도메인 기능을 조립하는 컴포넌트 기반 클래스다.
// 소유자(UPawn)는 WKPTR로만 보관해 UPawn(SHPTR) <-> UComponent(WKPTR) 순환 참조를 방지한다.
class UComponent abstract : public Core::UObject {
protected:
	explicit UComponent(Core::SHPTR<Core::UCoreInstance> coreInstance);
	virtual ~UComponent() override;

public:
	// 컴포넌트의 고유 유형 ID를 돌려준다. 0(EComponentType::None)은 등록할 수 없다.
	virtual Core::uint32 GetComponentTypeID() const noexcept = 0;

	// 현재 살아 있는 UPawn에 부착돼 있으면 true다.
	bool HasOwner() const noexcept { return !_Owner.expired(); }

	// UPawn::AddComponent에서 소유자를 바인딩할 때 호출한다.
	bool AttachToOwner(const Core::SHPTR<UPawn>& owner);

protected:
	// 현재 부착된 소유자 UPawn을 돌려준다. 부착 전이거나 소멸했으면 빈 포인터다.
	Core::SHPTR<UPawn> GetOwner() const noexcept { return _Owner.lock(); }

	// 소유자 UPawn에 부착된 직후 한 번 호출된다.
	virtual void OnAttached(const Core::SHPTR<UPawn>& owner);

private:
	NO_COPY(UComponent)

	Core::WKPTR<UPawn> _Owner;
};

SPACE_T
