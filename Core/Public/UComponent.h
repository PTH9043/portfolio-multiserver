#pragma once

#include "UObject.h"

SPACE_H(Core)

class UActor;

// 컴포넌트 종류를 식별하는 고정 키다. Core 기본 컴포넌트는 1~999, 상위 프로젝트(Server 등)는 UserStart 이후를 쓴다.
enum class EComponentType : uint32 {
	None = 0,
	Session = 1,
	UserStart = 1000
};

// UActor에 장착해 기능을 조립하는 컴포넌트 기반 클래스다.
// 소유자(UActor)는 WKPTR로만 보관해 UActor(SHPTR) <-> UComponent(WKPTR) 순환 참조를 방지한다.
class CORE_DLL UComponent abstract : public UObject {
protected:
	explicit UComponent(SHPTR<UCoreInstance> coreInstance);
	virtual ~UComponent() override;

public:
	// 컴포넌트의 고유 유형 ID를 돌려준다. 0(EComponentType::None)은 등록할 수 없다.
	virtual uint32 GetComponentTypeID() const noexcept = 0;

	// 현재 살아 있는 UActor에 장착돼 있으면 true다.
	bool HasOwner() const noexcept { return !_Owner.expired(); }

	// UActor::AddComponent에서 소유자를 바인딩할 때 호출한다(초기화 시점 전용).
	// owner가 비어 있거나 이미 살아 있는 다른 소유자에 붙어 있으면 false다.
	bool AttachToOwner(const SHPTR<UActor>& owner);

protected:
	// 현재 장착된 소유자 UActor를 돌려준다. 장착 전이거나 소유자가 소멸했으면 빈 포인터다.
	SHPTR<UActor> GetOwner() const noexcept { return _Owner.lock(); }

	// 소유자 UActor에 장착된 직후 한 번 호출된다. 파생 컴포넌트가 초기 바인딩이 필요할 때 재정의한다.
	virtual void OnAttached(const SHPTR<UActor>& owner);

private:
	NO_COPY(UComponent)

	WKPTR<UActor> _Owner;
};

SPACE_T
