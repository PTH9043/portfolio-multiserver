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

#include <iostream>

// 표준·concurrent·Boost.Asio 헤더는 CoreTypes.h와 CoreStlTypes.h가 포함한다.
#include "CoreTypes.h"

#include "CoreMacro.h"
#include "CoreStlTypes.h"
#include "UCoreGlobal.h"
#include "CustomString.h"
