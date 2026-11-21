#pragma once
#include "CustomMemory.h"

SPACE_H(Core)

// 프로세스 전역 싱글턴: 메모리 풀 관리자와 런타임 메트릭스 관리자를 보관한다.
class CORE_DLL UCoreGlobal final {
	DECLARE_INSTANCE(UCoreGlobal)
public:
	UMemoryAdiminstor& GetMemoryAdiminstor(){
		return _MemoryAdiminstor;
	}
#if defined(ENABLE_RUNTIME_METRICS)
	Metrics::URuntimeMetricsManager& GetRuntimeMetricsManager(){ return _RuntimeMetricsManager; }
#endif
private:
	UCoreGlobal() = default;
	~UCoreGlobal() = default;
private:
	UMemoryAdiminstor _MemoryAdiminstor;

#if defined(ENABLE_RUNTIME_METRICS)
	Metrics::URuntimeMetricsManager _RuntimeMetricsManager;
#endif
};


#define GCoreGlobal Core::UCoreGlobal::GetInstance()
#define GMemory GCoreGlobal.GetMemoryAdiminstor()

SPACE_T
