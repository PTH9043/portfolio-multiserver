#include "CoreDefine.h"
#include "UCoreBase.h"

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UCoreBase.cpp"

SPACE_H(Core)

UCoreBase::UCoreBase() = default;

// 파생형의 기본 복사 생성자도 새 번호를 받는다. 여기서 확인한 타입은 이 기반 부분이다.
UCoreBase::UCoreBase(const UCoreBase& other) noexcept
    : std::enable_shared_from_this<UCoreBase>{other}, _RuntimeTraceID{other._RuntimeTraceID}
{
    RUNTIME_TRACE_EVENT(Create, UCoreBase, UCoreBase, GetRuntimeTraceID());
}

UCoreBase::~UCoreBase() = default;

SPACE_T
