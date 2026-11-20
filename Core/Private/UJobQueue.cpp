#include "UJobQueue.h"

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UJobQueue.cpp"

SPACE_H(Core)

UJobQueue::UJobQueue()
{
    RUNTIME_TRACE_EVENT(Create, UJobQueue, UJobQueue, GetRuntimeTraceID());
}
UJobQueue::~UJobQueue()
{
    RUNTIME_TRACE_EVENT(Destroy, UJobQueue, ~UJobQueue, GetRuntimeTraceID());
}

SPACE_T
