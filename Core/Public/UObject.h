#pragma once
#include "UCoreBase.h"

SPACE_H(Core)
class UCoreInstance;

// UCoreInstance를 약참조(WKPTR)로 보관하는 Core 도메인 객체의 기반 클래스다.
class CORE_DLL UObject abstract : public UCoreBase {
protected:
	UObject(SHPTR<UCoreInstance> coreInstance);
	virtual ~UObject();

protected:
	SHPTR<UCoreInstance> GetCoreInstance() const {
		FRuntimeTraceScope runtimeScope{GetRuntimeTraceID(), {"UObject", "GetCoreInstance", "Core/Public/UObject.h", static_cast<Core::uint32>(__LINE__)}};
		return _CoreInstance.lock();
	}

private:
	WKPTR<UCoreInstance>	_CoreInstance;
};

SPACE_T
