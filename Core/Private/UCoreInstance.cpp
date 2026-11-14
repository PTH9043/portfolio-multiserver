#include "CoreDefine.h"
#include "UCoreInstance.h"
#include "UThreadManager.h"
#include "ULogger.h"

SPACE_H(Core)

bool UCoreInstance::Initialize()
{
    if (_ThreadManager || _NetworkManager || _Logger)
        return false;

    // shared_ptr이 이미 객체를 소유하므로 매니저에 자기 참조를 전달할 수 있다.
    // 모두 생성된 뒤에 멤버로 옮기므로 생성 예외가 나면 멤버는 비어 있다.
    const SHPTR<UCoreInstance> self = ThisShared<UCoreInstance>();
    SHPTR<UThreadManager> threadManager = MakeShared<UThreadManager>(self);
    SHPTR<UNetworkManager> networkManager = MakeShared<UNetworkManager>(self);
    SHPTR<ULogger> logger = MakeShared<ULogger>(self);

    _ThreadManager = std::move(threadManager);
    _NetworkManager = std::move(networkManager);
    _Logger = std::move(logger);
    return true;
}

bool UCoreInstance::RegisterWorkerThread(const uint32 threadNum, FUNC<void(EXPECTION_PTR)> onFailure, FUNC<void()> onComplete)
{
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
