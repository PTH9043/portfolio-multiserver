#include "CoreDefine.h"
#include "UObject.h"
#include "UCoreInstance.h"

SPACE_H(Core)

UObject::UObject(SHPTR<UCoreInstance> coreInstance) : _CoreInstance{coreInstance}
{

}

UObject::~UObject()
{}

SPACE_T