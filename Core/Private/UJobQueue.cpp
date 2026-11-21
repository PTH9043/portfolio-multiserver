#include "UJobQueue.h"

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UJobQueue.cpp"

SPACE_H(Core)

// 작업 큐의 생성·소멸을 런타임 트레이스에 기록한다.
UJobQueue::UJobQueue()
{
    RUNTIME_TRACE_EVENT(Create, UJobQueue, UJobQueue, GetRuntimeTraceID());
}
UJobQueue::~UJobQueue()
{
    RUNTIME_TRACE_EVENT(Destroy, UJobQueue, ~UJobQueue, GetRuntimeTraceID());
}

SPACE_T
