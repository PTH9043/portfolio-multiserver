#pragma once
#include "RuntimeTrace.h"

SPACE_H(Core)

// 서버에서 객체 역할을 하는 클래스의 최상위 부모다. (GraduateProject 의 ACoreBase 를 이은 것)
class CORE_DLL UCoreBase abstract : public std::enable_shared_from_this<UCoreBase> {
protected:
	UCoreBase();
	UCoreBase(const UCoreBase& other) noexcept;
	virtual ~UCoreBase();
	uint64 GetRuntimeTraceID() const noexcept { return _RuntimeTraceID.Get(); }
public:
	UCoreBase& operator=(const UCoreBase&) = default;
	// 자기 자신을 T 의 SHPTR / WKPTR 로 얻는다. T 는 이 객체의 실제 타입이거나 그 부모여야 한다.
	template<class T>
	SHPTR<T> ThisShared() { return std::static_pointer_cast<T>(shared_from_this()); }

	// ThisShared 와 달리 예외를 던지지 않는다. 소유자가 없으면 빈 WKPTR 을 돌려준다.
	// 일감(람다)이 자기 큐를 붙잡을 때는 SHPTR 대신 이것을 붙잡는다. (SHPTR 로 붙잡으면 순환 참조로 영영 해제되지 않는다)
	template<class T>
	WKPTR<T> ThisWeak() { return WKPTR<T>{ std::static_pointer_cast<T>(weak_from_this().lock()) }; }
private:
	FRuntimeTraceID _RuntimeTraceID;
};

SPACE_T
