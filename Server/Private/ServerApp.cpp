#include "ServerApp.h"
#include "UCoreInstance.h"
#include "UNetworkManager.h"
#include "UThreadManager.h"
#include "ULogger.h"

#include <exception>
#include <stdexcept>

SPACE_H(Server)

// Logger는 RegisterWorkers에서 접수를 시작하지만, 로그 스레드는 JoinWorkers(또는 Stop)의 JoinAll이
// 실행 신호를 준 뒤에야 돌고 모든 생산자가 끝나면 종료한다.
// Start·RegisterWorkers의 오류와 Stop의 상태 출력은 로그 스레드가 아직 돌지 않거나 종료(또는 종료 대기)
// 단계라 Logger 출력이 보장되지 않으므로 콘솔에 직접 쓴다.

namespace {
const char* GetLogLevelName(const ELogLevel level) noexcept
{
	switch (level) {
	case ELogLevel::Error: return "ERROR";
	case ELogLevel::Warning: return "WARNING";
	default: return "INFO";
	}
}
}

UServerApp::~UServerApp()
{
	Stop();
	// 워커가 남아 있을 수 있으면 워커가 참조하는 객체를 해제하지 않는다.
	if (_bRunning.load() && _WorkersRegistered)
		terminate();

	ClearObject();
}

bool UServerApp::Start()
{
	RETURN_INVALID_COND(_bRunning.load(), false);

	try {
		_NetworkFailure = nullptr;
		_LoggerFailure = nullptr;
		_InputFailure.clear();
		_NetworkFailed.store(false, std::memory_order_relaxed);
		_InputFailed.store(false);
		_LoggerFailed.store(false);
		_RemainingLogProducers.store(0);
		_StopState.store(EStopState::NotRequested, std::memory_order_relaxed);

		_CoreInstance = MakeShared<UCoreInstance>();
		if (!_CoreInstance->Initialize()) {
			std::cerr << "CoreInstance initialization failed or was already initialized.\n";
			return false;
		}

		_ServerInput = std::make_unique<UServerInput>();
		if (!_ServerInput->IsReady()) {
			std::cerr << "Server input initialization failed: "
				<< _ServerInput->GetLastError().message() << '\n';
			ClearObject();
			return false;
		}
	}
	catch (const std::exception& error) {
		std::cerr << "CoreInstance initialization failed: " << error.what() << '\n';
		ClearObject();
		return false;
	}
	catch (...) {
		std::cerr << "CoreInstance initialization failed with an unknown exception.\n";
		ClearObject();
		return false;
	}

	_bRunning.store(true);
	std::cout << "Server Start\n";
	return true;
}

bool UServerApp::RegisterWorkers(uint32 networkWorkerCount)
{
	if (!_bRunning.load() || _WorkersRegistered || networkWorkerCount == 0)
		return false;

	SHPTR<UThreadManager> threadManager = _CoreInstance->GetThreadManager();
	if (!threadManager)
		return false;

	// 일부 등록 후 실패해도 종료 대기가 끝나기 전까지 객체를 유지한다.
	_WorkersRegistered = true;
	_RemainingLogProducers.store(static_cast<uint64>(networkWorkerCount) + 1, std::memory_order_release);
	try {
		if (!_CoreInstance->RegisterWorkerThread(networkWorkerCount, 
			[this](EXPECTION_PTR failure) noexcept { ReportFailure(_NetworkFailed, _NetworkFailure, failure); },
			[this]() noexcept { LogProducerFinished(); }))
			return false;

		if (!threadManager->RegisterFunc(RunInputWorker, this))
			return false;

		const auto logger = _CoreInstance->GetLogger();
		if (!logger)
			return false;
		if (!logger->Start(
			[](const LOGBATCH_CONTAINER& batch) {
				for (const auto& record : batch) {
					std::cout << '[' << record->QueueSequenceNum << "][" << GetLogLevelName(record->Level) << "] "
						<< record->Message.View() << '\n';
				}
				std::cout << std::flush;
				if (!std::cout)
					throw std::runtime_error("Log console output failed");
			},
			[this](EXPECTION_PTR failure) noexcept { ReportFailure(_LoggerFailed, _LoggerFailure, failure); }))
			return false;

		if (!threadManager->RegisterFunc(ULogger::RunWorker, logger.get()))
			return false;
	}
	catch (const std::exception& error) {
		std::cerr << "Worker registration failed: " << error.what() << '\n';
		return false;
	}
	catch (...) {
		std::cerr << "Worker registration failed with an unknown exception.\n";
		return false;
	}

	return true;
}

bool UServerApp::JoinWorkers()
{
	if (!_bRunning.load() || !_WorkersRegistered)
		return false;

	SHPTR<UThreadManager> threadManager = _CoreInstance->GetThreadManager();
	if (!threadManager)
		return false;
	if (!threadManager->JoinAll())
		return false;

	_WorkersRegistered = false;
	return true;
}

void UServerApp::Stop()
{
	RETURN_INVALID_COND(false == _bRunning.load(), );

	RequestStop();
	EStopState stopState = _StopState.load(std::memory_order_acquire);
	while (stopState == EStopState::Requesting) {
		_StopState.wait(EStopState::Requesting, std::memory_order_acquire);
		stopState = _StopState.load(std::memory_order_acquire);
	}
	if (stopState == EStopState::Failed) {
		std::cerr << "Failed to request server shutdown.\n";
		return;
	}

	SHPTR<UThreadManager> threadManager = _CoreInstance->GetThreadManager();
	if (!threadManager) {
		std::cerr << "Thread manager unavailable; cannot confirm server thread shutdown.\n";
		return;
	}
	if (!threadManager->JoinAll()) {
		std::cerr << "Failed to join server threads.\n";
		return;
	}
	_bRunning.store(false);
	_WorkersRegistered = false;
	std::cout << "Server Stop\n";
}

void UServerApp::RequestStop() noexcept
{
	if (!_bRunning.load(std::memory_order_acquire))
		return;

	EStopState expected = EStopState::NotRequested;
	if (!_StopState.compare_exchange_strong(expected, EStopState::Requesting,
		std::memory_order_acq_rel))
		return;

	EStopState result = EStopState::Requested;
	// 위의 상태 교환으로 이 함수 본문은 한 번만 실행되므로 work guard 해제도 한 번만 일어난다.
	const auto networkManager = _CoreInstance->GetNetworkManager();
	if (networkManager)
		networkManager->ReleaseWork();

	if (_ServerInput && !_ServerInput->RequestStop())
		result = EStopState::Failed;

	_StopState.store(result, std::memory_order_release);
	_StopState.notify_all();
}

EXPECTION_PTR UServerApp::GetNetworkFailure() const
{
	SHARED_LOCK_GUARD lock{ _FailureMutex };
	return _NetworkFailure;
}

EXPECTION_PTR UServerApp::GetLoggerFailure() const
{
	SHARED_LOCK_GUARD lock{ _FailureMutex };
	return _LoggerFailure;
}

ERROR_CODE UServerApp::GetInputFailure() const
{
	SHARED_LOCK_GUARD lock{ _FailureMutex };
	return _InputFailure;
}

void UServerApp::RunInputWorker(void* data) noexcept
{
	UServerApp* server = static_cast<UServerApp*>(data);
	UServerInput& input = *server->_ServerInput;
	const UServerInput::EResult result = input.Wait();
	if (result == UServerInput::EResult::Error) {
		server->_InputFailed.store(true);
		EXCLUSIVE_LOCK_GUARD lock{ server->_FailureMutex };
		server->_InputFailure = input.GetLastError();
	}
	// Enter, 입력 종료, 입력 오류는 모두 서버 종료를 요청한다.
	if (result != UServerInput::EResult::StopRequested)
		server->RequestStop();
	server->LogProducerFinished();
}

void UServerApp::ReportFailure(ATOMIC<bool>& failed, EXPECTION_PTR& storedFailure, EXPECTION_PTR failure) noexcept
{
	failed.store(true, std::memory_order_release);
	{
		// 처음 보고된 원인만 보관한다. 종료 요청은 잠금 밖에서 한다.
		EXCLUSIVE_LOCK_GUARD lock{ _FailureMutex };
		if (!storedFailure)
			storedFailure = failure;
	}
	RequestStop();
}

void UServerApp::LogProducerFinished() noexcept
{
	if (_RemainingLogProducers.fetch_sub(1, std::memory_order_acq_rel) != 1)
		return;
	const auto logger = _CoreInstance->GetLogger();
	if (logger)
		logger->RequestStop();
}

void UServerApp::ClearObject()
{
	_ServerInput.reset();
	_CoreInstance.reset();
}

SPACE_T
