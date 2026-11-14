#pragma once
#include "UCoreBase.h"

SPACE_H(Core)
class UCoreInstance;

class CORE_DLL UObject abstract : public UCoreBase {
protected:
	UObject(SHPTR<UCoreInstance> coreInstance);
	virtual ~UObject();

protected:
	SHPTR<UCoreInstance> GetCoreInstance() const { return _CoreInstance.lock(); }

private:
	WKPTR<UCoreInstance>	_CoreInstance;
};

SPACE_T
