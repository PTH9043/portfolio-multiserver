#include "ServerApp.h"
#include "ChatProtocolLimits.h"
#include "UGameRoom.h"
#include "UServerListener.h"
#include "UCoreInstance.h"
#include "UNetworkManager.h"
#include "URoomManager.h"
#include "UThreadManager.h"
#include "ULogger.h"

#include <boost/system/system_error.hpp>
#include <exception>
#include <stdexcept>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/ServerApp.cpp"

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

void AppendJsonEscaped(std::string& output, const STRINGVIEW text)
{
	static constexpr char HEX_DIGITS[] = "0123456789abcdef";
	for (const char ch : text) {
		const uint8 byte = static_cast<uint8>(ch);
		switch (ch) {
		case '"': output.append("\\\""); break;
		case '\\': output.append("\\\\"); break;
		case '\b': output.append("\\b"); break;
		case '\f': output.append("\\f"); break;
		case '\n': output.append("\\n"); break;
		case '\r': output.append("\\r"); break;
		case '\t': output.append("\\t"); break;
		default:
			if (byte < 0x20U) {
				output.append("\\u00");
				output.push_back(HEX_DIGITS[(byte >> 4) & 0x0FU]);
				output.push_back(HEX_DIGITS[byte & 0x0FU]);
			}
			else {
				output.push_back(ch);
			}
			break;
		}
	}
}

std::string FormatRuntimeLogBatch(const LOGBATCH_CONTAINER& batch)
{
	std::string payload;
	payload.reserve(static_cast<uint64>(batch.size()) * 160ULL);
	for (const auto& record : batch) {
		if (!record)
			continue;
		const int64 epochMs = std::chrono::duration_cast<MILLISECONDS>(
			record->OccurredAtTime.time_since_epoch()).count();
		payload.append("{\"seq\":");
		payload.append(std::to_string(record->QueueSequenceNum));
		payload.append(",\"time\":");
		payload.append(std::to_string(epochMs));
		payload.append(",\"level\":\"");
		payload.append(GetLogLevelName(record->Level));
		payload.append("\",\"thread\":");
		payload.append(std::to_string(record->ProducerOSThreadID));
		payload.append(",\"traceSeq\":");
		payload.append(std::to_string(record->TraceSequence));
		payload.append(",\"objectId\":\"");
		payload.append(std::to_string(record->TraceObjectID));
		payload.append("\",\"msg\":\"");
		AppendJsonEscaped(payload, record->Message.View());
		payload.append("\"}\n");
	}
	return payload;
}
}

UServerApp::UServerApp()
{
	RUNTIME_TRACE_EVENT(Create, UServerApp, UServerApp, _RuntimeTraceID.Get());
}

UServerApp::~UServerApp()
{
	RUNTIME_TRACE_SCOPE(UServerApp, ~UServerApp, _RuntimeTraceID.Get());
	Stop();
	// 워커가 남아 있을 수 있으면 워커가 참조하는 객체를 해제하지 않는다.
	if (_bRunning.load() && _WorkersRegistered)
		terminate();

	ClearObject();
	RUNTIME_TRACE_EVENT(Destroy, UServerApp, ~UServerApp, _RuntimeTraceID.Get());
}

bool UServerApp::Start(const uint16 port, const MILLISECONDS sessionIdleTimeout)
{
	RUNTIME_TRACE_SCOPE(UServerApp, Start, _RuntimeTraceID.Get());
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

		// 고정 채널 4개와 전역 닉네임 레지스트리는 리스너가 접속을 받기 전에 먼저 등록해 둔다.
		const auto networkManager = _CoreInstance->GetNetworkManager();
		const auto roomManager = _CoreInstance->GetRoomManager();
		if (!networkManager || !roomManager) {
			std::cerr << "Core managers unavailable.\n";
			ClearObject();
			return false;
		}
		_NicknameRegistry = MakeShared<UNicknameRegistry>();
		for (const FFixedRoomSpec& spec : FIXED_GAME_ROOMS) {
			const auto room = MakeShared<UGameRoom>(
				_CoreInstance,
				networkManager->GetIOContext(),
				spec.RoomID,
				DEFAULT_ROOM_MAX_USERS,
				FString{ spec.Name });
			if (!roomManager->AddRoom(room)) {
				std::cerr << "Fixed game room registration failed (roomID=" << spec.RoomID << ").\n";
				ClearObject();
				return false;
			}
		}

		// 리스너는 네트워크 워커를 등록하기 전에 시작해야 한다. 지금은 이 PC(127.0.0.1)의 접속만 받는다.
		BOOST_ERROR_CODE error;
		const bool started = networkManager->StartListener(
			MakeShared<UServerListener>(_CoreInstance, networkManager->GetIOContext(), _NicknameRegistry, sessionIdleTimeout),
			TCP_ENDPOINT{ ASIO::ip::address_v4::loopback(), port },
			[this](const BOOST_ERROR_CODE& acceptError) {
				// 리스너는 accept 오류 뒤 새 접속을 받지 않으므로 서버를 끝낸다. 흔한 접속 중단은 Boost가 다시 받는다.
				const auto logger = _CoreInstance->GetLogger();
				if (logger)
					logger->TryLogError("UServerListener accept failed", acceptError);
				ReportFailure(_NetworkFailed, _NetworkFailure,
					std::make_exception_ptr(boost::system::system_error{ acceptError, "UServerListener accept failed" }));
			},
			error);
		if (!started) {
			std::cerr << "Listener start failed on 127.0.0.1:" << port << ": " << error.message() << '\n';
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

	_ListenPort = port;
	_bRunning.store(true);
	std::cout << "Server Start\n";
	return true;
}

bool UServerApp::RegisterWorkers(uint32 networkWorkerCount)
{
	RUNTIME_TRACE_SCOPE(UServerApp, RegisterWorkers, _RuntimeTraceID.Get());
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
				const std::string jsonBatch = FormatRuntimeLogBatch(batch);
				AppendRuntimeLogBatch(STRINGVIEW{ jsonBatch.data(), jsonBatch.size() });
			},
			[this](EXPECTION_PTR failure) noexcept { ReportFailure(_LoggerFailed, _LoggerFailure, failure); }))
			return false;

		{
			FString readyMessage{ "Server ready on 127.0.0.1:" };
			readyMessage << std::to_string(_ListenPort) << " (workers=" << std::to_string(networkWorkerCount) << ")";
			logger->TryLog(ELogLevel::Info, readyMessage.View());
		}

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
	RUNTIME_TRACE_SCOPE(UServerApp, JoinWorkers, _RuntimeTraceID.Get());
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
	RUNTIME_TRACE_SCOPE(UServerApp, Stop, _RuntimeTraceID.Get());
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
	RUNTIME_TRACE_SCOPE(UServerApp, RequestStop, _RuntimeTraceID.Get());
	if (!_bRunning.load(std::memory_order_acquire))
		return;

	EStopState expected = EStopState::NotRequested;
	if (!_StopState.compare_exchange_strong(expected, EStopState::Requesting,
		std::memory_order_acq_rel))
		return;

	EStopState result = EStopState::Requested;
	// 위의 상태 교환으로 이 함수 본문은 한 번만 실행되므로 네트워크 종료도 한 번만 일어난다.
	const auto networkManager = _CoreInstance->GetNetworkManager();
	if (networkManager)
		networkManager->Shutdown();

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
	RUNTIME_TRACE_SCOPE(UServerApp, RunInputWorker, server->_RuntimeTraceID.Get());
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
	RUNTIME_TRACE_SCOPE(UServerApp, ReportFailure, _RuntimeTraceID.Get());
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
	RUNTIME_TRACE_SCOPE(UServerApp, LogProducerFinished, _RuntimeTraceID.Get());
	if (_RemainingLogProducers.fetch_sub(1, std::memory_order_acq_rel) != 1)
		return;
	const auto logger = _CoreInstance->GetLogger();
	if (logger)
		logger->RequestStop();
}

void UServerApp::ClearObject()
{
	RUNTIME_TRACE_SCOPE(UServerApp, ClearObject, _RuntimeTraceID.Get());
	_ServerInput.reset();
	_NicknameRegistry.reset();
	_CoreInstance.reset();
}

SPACE_T
