#pragma once
#include "CustomMemory.h"

SPACE_H(Core)

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
