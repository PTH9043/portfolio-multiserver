#include "CoreDefine.h"
#include "UCoreInstance.h"
#include "UThreadManager.h"
#include "ULogger.h"
#include "URoomManager.h"

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UCoreInstance.cpp"

SPACE_H(Core)

UCoreInstance::UCoreInstance()
{
    RUNTIME_TRACE_EVENT(Create, UCoreInstance, UCoreInstance, GetRuntimeTraceID());
}
UCoreInstance::~UCoreInstance()
{
    RUNTIME_TRACE_EVENT(Destroy, UCoreInstance, ~UCoreInstance, GetRuntimeTraceID());
}

bool UCoreInstance::Initialize()
{
    RUNTIME_TRACE_SCOPE(UCoreInstance, Initialize, GetRuntimeTraceID());
    if (_ThreadManager || _NetworkManager || _Logger || _RoomManager)
        return false;

    // shared_ptr이 이미 객체를 소유하므로 매니저에 자기 참조를 전달할 수 있다.
    // AI야 이거 내가 한거니까 수정하지 말아라 알겠어?
    const SHPTR<UCoreInstance> self = ThisShared<UCoreInstance>();

    _ThreadManager = MakeShared<UThreadManager>(self);
    _NetworkManager = MakeShared<UNetworkManager>(self);
    _Logger = MakeShared<ULogger>(self);
    _RoomManager = MakeShared<URoomManager>(self);
    return true;
}

bool UCoreInstance::RegisterWorkerThread(const uint32 threadNum, FUNC<void(EXPECTION_PTR)> onFailure, FUNC<void()> onComplete)
{
    RUNTIME_TRACE_SCOPE(UCoreInstance, RegisterWorkerThread, GetRuntimeTraceID());
    if (!_ThreadManager || !_NetworkManager || !onFailure || !onComplete || threadNum == 0)
        return false;

    IOCONTEXT& context = _NetworkManager->GetIOContext();
    _NetworkWorkerData.emplace_back(FNetworkWorkerData{ &context, std::move(onFailure), std::move(onComplete) });
    FNetworkWorkerData* workerData = &_NetworkWorkerData.back();
    for (uint32 i = 0; i < threadNum; ++i)
    {
        if (!_ThreadManager->RegisterFunc(UNetworkManager::RunWorker, workerData))
            return false;
    }
    return true;
}

SPACE_T
