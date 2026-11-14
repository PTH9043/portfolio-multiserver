#pragma once

#pragma warning (disable : 4251)
#pragma warning (disable : 6031)
#pragma warning (disable : 4150)
#pragma warning (disable : 4200)



#ifdef _WIN32
#define _WIN32_WINNT 0x0A00
#else
#include <stdlib.h>
#include <sys/mman.h>
#endif


#ifdef CORE_EXPORTS
#define CORE_DLL   _declspec(dllexport)
#else
#define CORE_DLL  _declspec(dllimport)
#endif

// 서버를 측정하기 위한 매크로
// 런타임 메트릭을 활성화하면 서버의 메모리 할당과 관련된 통계를 수집할 수 있다.
#define ENABLE_RUNTIME_METRICS

// 메모리 풀 관련 통계를 수집하기 위한 매크로
#define ENABLE_MP_RUNTIME_METRICS


#include <iostream>

// 표준·concurrent·Boost.Asio 헤더는 CoreTypes.h와 CoreStlTypes.h가 포함한다.
#include "CoreTypes.h"

#include "CoreMacro.h"
#include "CoreStlTypes.h"
#include "RunTimeMetrics.h"
#include "MPMetrics.h"
#include "UCoreGlobal.h"
#include "CustomString.h"
