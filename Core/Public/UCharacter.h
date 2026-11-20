#pragma once

#include "UPawn.h"

SPACE_H(Core)

// 스스로 움직일 수 있는 사물의 기반 클래스다.
class CORE_DLL UCharacter abstract : public UPawn {
protected:
	explicit UCharacter(SHPTR<UCoreInstance> coreInstance);
	virtual ~UCharacter() override;

private:
	NO_COPY(UCharacter)
};

SPACE_T
