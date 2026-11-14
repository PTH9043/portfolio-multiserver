#pragma once
#include "CoreDefine.h"

SPACE_H(Core)

// Server ID 발급기
// 정의는 IDGenerator.cpp 에 있다. (헤더에 두면 include 하는 파일마다 정의가 생겨 링크 오류가 나고,
// CORE_DLL 이 없으면 exe 와 DLL 이 서로 다른 카운터를 갖게 된다)
class CORE_DLL UIDGenerator {
public:
	static SERVERID ServerNextID();

private:
	static ATOMIC<SERVERID> _idIssuance;
};

SPACE_T
