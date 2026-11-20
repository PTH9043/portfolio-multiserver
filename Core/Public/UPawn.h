#pragma once

#include "UActor.h"

SPACE_H(Core)

// 화면에 그려지지만 스스로 움직일 수 없는 사물의 기반 클래스다.
class CORE_DLL UPawn abstract : public UActor {
protected:
	explicit UPawn(SHPTR<UCoreInstance> coreInstance);
	virtual ~UPawn() override;

private:
	NO_COPY(UPawn)
};

SPACE_T
