#include "CoreDefine.h"
#include "UNetworkManager.h"

SPACE_H(Core)

UNetworkManager::UNetworkManager(SHPTR<UCoreInstance> coreInstance) : 
	UObject{ coreInstance },
	_WorkGuard{ ASIO::make_work_guard(_IOContext) }
{
}

UNetworkManager::~UNetworkManager() = default;

void UNetworkManager::RunWorker(void* Data)
{
	FNetworkWorkerData* workerData = static_cast<FNetworkWorkerData*>(Data);
	EXPECTION_PTR callbackFailure;
	try {
		workerData->Context->run();
	}
	catch (...) {
		try {
			workerData->OnFailure(std::current_exception());
		}
		catch (...) {
			callbackFailure = std::current_exception();
		}
	}
	workerData->OnComplete();
	if (callbackFailure)
		std::rethrow_exception(callbackFailure);
}

void UNetworkManager::ReleaseWork() noexcept
{
	_WorkGuard.reset();
}

SPACE_T
