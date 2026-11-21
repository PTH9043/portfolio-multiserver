#include "CoreDefine.h"
#include "UObject.h"
#include "UCoreInstance.h"

SPACE_H(Core)

// 소속 UCoreInstance를 약참조로 연결해 순환 참조를 방지한다.
UObject::UObject(SHPTR<UCoreInstance> coreInstance) : _CoreInstance{coreInstance}
{

}

UObject::~UObject()
{}

SPACE_T