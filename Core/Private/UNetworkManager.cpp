#include "CoreDefine.h"
#include "UNetworkManager.h"
#include "UListener.h"
#include "USession.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include <boost/asio/post.hpp>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UNetworkManager.cpp"

SPACE_H(Core)

UNetworkManager::UNetworkManager(SHPTR<UCoreInstance> coreInstance) : 
	UObject{ coreInstance },
	_WorkGuard{ ASIO::make_work_guard(_IOContext) },
	_Strand{ ASIO::make_strand(_IOContext) }
{
	RUNTIME_TRACE_EVENT(Create, UNetworkManager, UNetworkManager, GetRuntimeTraceID());
}

UNetworkManager::~UNetworkManager()
{
	RUNTIME_TRACE_EVENT(Destroy, UNetworkManager, ~UNetworkManager, GetRuntimeTraceID());
}

void UNetworkManager::RunWorker(void* Data)
{
	RUNTIME_TRACE_SCOPE(UNetworkManager, RunWorker, 0);
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

void UNetworkManager::Shutdown() noexcept
{
	RUNTIME_TRACE_SCOPE(UNetworkManager, Shutdown, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;

	// 새 접속을 먼저 막는다. 이미 수락돼 등록 중인 접속은 _Closing으로 닫힌다.
	if (_Listener) {
		BOOST_ERROR_CODE stopError;
		_Listener->Stop(stopError);
		if (stopError && logger)
			logger->TryLogError("UNetworkManager::Shutdown listener stop failed", stopError);
	}

	try {
		RequestCloseSessions();
	}
	catch (...) {
		// 게시하지 못하면 Session을 닫을 수 없어 대기 중인 수신 때문에 run()이 끝나지 않는다. 워커를 강제로 멈춘다.
		if (logger)
			logger->TryLog(ELogLevel::Error, "UNetworkManager::Shutdown could not post session close; stopping io_context");
		try {
			_IOContext.stop();
		}
		catch (const boost::system::system_error& error) {
			// stop은 중지 표시를 먼저 켜므로 실패해도 워커는 대기 시간이 지나면 중지를 알아챈다.
			if (logger)
				logger->TryLogError("UNetworkManager::Shutdown io_context stop failed", error.code());
		}
	}

	// 위에서 게시한 작업도 남은 작업이므로 바로 해제해도 세션 닫기보다 먼저 run()이 끝나지 않는다.
	ReleaseWork();
}

bool UNetworkManager::StartListener(SHPTR<UListener> listener, const TCP_ENDPOINT& endpoint,
	FUNC<void(const BOOST_ERROR_CODE&)> onFailure, BOOST_ERROR_CODE& error)
{
	RUNTIME_TRACE_SCOPE(UNetworkManager, StartListener, GetRuntimeTraceID());
	error.clear();
	if (!listener) {
		error = ASIO::error::invalid_argument;
		return false;
	}
	// 워커 등록 전 제어 스레드에서만 호출하므로 _Listener에 잠금을 걸지 않는다.
	if (_Listener) {
		error = ASIO::error::already_started;
		return false;
	}

	// 시작에 실패한 리스너는 보관하지 않는다. 보관한 리스너는 Shutdown이 멈춘다.
	if (!listener->Start(endpoint, std::move(onFailure), error))
		return false;
	_Listener = std::move(listener);
	return true;
}

void UNetworkManager::RequestAddSession(const SHPTR<USession>& session)
{
	RUNTIME_TRACE_SCOPE(UNetworkManager, RequestAddSession, GetRuntimeTraceID());
	if (!session)
		return;

	// 매니저를 SHPTR로 붙잡으면 _IOContext에 남은 작업이 매니저를 소유해 순환 참조가 된다.
	// 게시한 작업은 네트워크 워커만 실행하고, 워커는 매니저가 소멸하기 전에 모두 join되므로 this로 충분하다.
	const auto submission = RUNTIME_TRACE_EVENT(Submit, UNetworkManager, RequestAddSession, GetRuntimeTraceID());
	ASIO::post(_Strand, [this, session, submission] {
		RUNTIME_TRACE_CALLBACK(UNetworkManager, RequestAddSession, GetRuntimeTraceID(), submission);
		// 전체 닫기가 이미 목록을 돌았으므로 목록에 넣으면 아무도 닫지 않아 네트워크 워커가 끝나지 않는다.
		if (_Closing) {
			session->Close();
			return;
		}
		_SessionContainer.insert(session);
	});
}

void UNetworkManager::RequestRemoveSession(const SHPTR<USession>& session)
{
	RUNTIME_TRACE_SCOPE(UNetworkManager, RequestRemoveSession, GetRuntimeTraceID());
	if (!session)
		return;

	const auto submission = RUNTIME_TRACE_EVENT(Submit, UNetworkManager, RequestRemoveSession, GetRuntimeTraceID());
	ASIO::post(_Strand, [this, session, submission] {
		RUNTIME_TRACE_CALLBACK(UNetworkManager, RequestRemoveSession, GetRuntimeTraceID(), submission);
		_SessionContainer.erase(session);
	});
}

void UNetworkManager::RequestCloseSessions()
{
	RUNTIME_TRACE_SCOPE(UNetworkManager, RequestCloseSessions, GetRuntimeTraceID());
	const auto submission = RUNTIME_TRACE_EVENT(Submit, UNetworkManager, RequestCloseSessions, GetRuntimeTraceID());
	ASIO::post(_Strand, [this, submission] {
		RUNTIME_TRACE_CALLBACK(UNetworkManager, RequestCloseSessions, GetRuntimeTraceID(), submission);
		_Closing = true;
		// Close의 목록 제거 요청은 post라서 이 순회가 끝난 뒤 실행되므로 반복자가 무효화되지 않는다.
		for (const auto& session : _SessionContainer)
			session->Close();
	});
}

void UNetworkManager::ReleaseWork() noexcept
{
	RUNTIME_TRACE_SCOPE(UNetworkManager, ReleaseWork, GetRuntimeTraceID());
	_WorkGuard.reset();
}

SPACE_T
