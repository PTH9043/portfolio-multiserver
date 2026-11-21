#include "UDatabaseManager.h"
#include "UCoreInstance.h"
#include "UThreadManager.h"
#include "ULogger.h"

#include <mysql.h>
#include <mutex>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UDatabaseManager.cpp"

SPACE_H(Server)

UDatabaseManager::UDatabaseManager(Core::SHPTR<Core::UCoreInstance> coreInstance, FDbConfig config)
	: UObject{ std::move(coreInstance) },
	  _Config{ std::move(config) }
{
	RUNTIME_TRACE_EVENT(Create, UDatabaseManager, UDatabaseManager, GetRuntimeTraceID());
}

UDatabaseManager::~UDatabaseManager()
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, ~UDatabaseManager, GetRuntimeTraceID());
	RequestStop();
	if (_LibraryInitialized) {
		mysql_library_end();
		_LibraryInitialized = false;
	}
	RUNTIME_TRACE_EVENT(Destroy, UDatabaseManager, ~UDatabaseManager, GetRuntimeTraceID());
}

bool UDatabaseManager::Initialize(Core::FString& outError)
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, Initialize, GetRuntimeTraceID());
	if (!_Config.Enabled)
		return true;

	if (_Config.WorkerCount == 0)
		_Config.WorkerCount = 4;

	if (mysql_library_init(0, nullptr, nullptr) != 0) {
		outError = Core::FString{ "mysql_library_init failed" };
		return false;
	}
	_LibraryInitialized = true;

	// 기동 단계에서 MySQL 접속과 스키마 접근 가능 여부를 사전 검증한다.
	FMySqlConnection probe{ _Config };
	if (!probe.EnsureConnected(outError))
		return false;
	probe.Close();
	return true;
}

bool UDatabaseManager::RegisterWorkers(const Core::SHPTR<Core::UThreadManager>& threadManager,
	FWorkerFinishedCallback onWorkerFinished)
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, RegisterWorkers, GetRuntimeTraceID());
	if (!_Config.Enabled)
		return true;
	if (!threadManager || !_LibraryInitialized)
		return false;

	_OnWorkerFinished = std::move(onWorkerFinished);
	for (Core::uint32 i = 0; i < _Config.WorkerCount; ++i) {
		if (!threadManager->RegisterFunc(UDatabaseManager::RunWorker, this))
			return false;
	}
	return true;
}

void UDatabaseManager::RequestStop() noexcept
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, RequestStop, GetRuntimeTraceID());
	{
		Core::EXCLUSIVE_LOCK_GUARD lock{ _QueueMutex };
		if (_StopRequested)
			return;
		_StopRequested = true;
	}
	_QueueCV.notify_all();
}

bool UDatabaseManager::EnqueueRegister(Core::FString username, Core::FString password, Core::FString nickname,
	FRegisterCallback callback)
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, EnqueueRegister, GetRuntimeTraceID());
	if (!_Config.Enabled || !callback)
		return false;

	const Core::uint64 submitSeq = RUNTIME_TRACE_EVENT(Submit, UDatabaseManager, EnqueueRegister, GetRuntimeTraceID());
	{
		Core::EXCLUSIVE_LOCK_GUARD lock{ _QueueMutex };
		if (_StopRequested || _TaskQueue.size() >= DB_MAX_AUTH_PENDING_TASKS)
			return false;
		FRegisterTask payload{ std::move(username), std::move(password), std::move(nickname), std::move(callback) };
		_TaskQueue.push_back(FDbTask{ std::move(payload), submitSeq });
	}
	_QueueCV.notify_one();
	return true;
}

bool UDatabaseManager::EnqueueLogin(Core::FString username, Core::FString password, Core::FString remoteIp,
	FLoginCallback callback)
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, EnqueueLogin, GetRuntimeTraceID());
	if (!_Config.Enabled || !callback)
		return false;

	const Core::uint64 submitSeq = RUNTIME_TRACE_EVENT(Submit, UDatabaseManager, EnqueueLogin, GetRuntimeTraceID());
	{
		Core::EXCLUSIVE_LOCK_GUARD lock{ _QueueMutex };
		if (_StopRequested || _TaskQueue.size() >= DB_MAX_AUTH_PENDING_TASKS)
			return false;
		FLoginTask payload{ std::move(username), std::move(password), std::move(remoteIp), std::move(callback) };
		_TaskQueue.push_back(FDbTask{ std::move(payload), submitSeq });
	}
	_QueueCV.notify_one();
	return true;
}

bool UDatabaseManager::EnqueueLogout(const Core::uint64 logId, const Core::uint32 lastRoomId,
	Core::FString disconnectReason)
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, EnqueueLogout, GetRuntimeTraceID());
	if (!_Config.Enabled || logId == 0)
		return false;

	const Core::uint64 submitSeq = RUNTIME_TRACE_EVENT(Submit, UDatabaseManager, EnqueueLogout, GetRuntimeTraceID());
	{
		Core::EXCLUSIVE_LOCK_GUARD lock{ _QueueMutex };
		// 로그아웃 기록은 종료 직전 네트워크 정리 중에도 들어올 수 있으므로 일반 인증 상한보다 넓은 여유를 둔다.
		if (_StopRequested || _TaskQueue.size() >= DB_MAX_TOTAL_PENDING_TASKS)
			return false;
		FLogoutTask payload{ logId, lastRoomId, std::move(disconnectReason) };
		_TaskQueue.push_back(FDbTask{ std::move(payload), submitSeq });
	}
	_QueueCV.notify_one();
	return true;
}

void UDatabaseManager::RunWorker(void* data) noexcept
{
	auto* manager = static_cast<UDatabaseManager*>(data);
	if (!manager)
		return;
	manager->WorkerLoop();
}

void UDatabaseManager::WorkerLoop() noexcept
{
	const Core::FRuntimeTraceID workerTrace;
	RUNTIME_TRACE_SCOPE(UDatabaseManager, WorkerLoop, workerTrace.Get());

	// DB 전용 스레드 1개가 자기 전용 MySQL 커넥션 1개를 RAII로 영구 소유한다(커넥션 락 경합 0).
	{
		FMySqlConnection connection{ _Config };
		Core::FString connectError;
		if (!connection.EnsureConnected(connectError))
			LogDbWarning("WorkerLoop initial connect", connectError.View());

		for (;;) {
			FDbTask task;
			{
				std::unique_lock<Core::SHARED_MUTEX> lock{ _QueueMutex };
				_QueueCV.wait(lock, [this]() { return _StopRequested || !_TaskQueue.empty(); });
				// 종료 신호가 와도 큐에 남은 로그아웃 기록 등을 끝까지 배수(Drain)한 뒤 종료한다.
				if (_TaskQueue.empty())
					break;
				task = std::move(_TaskQueue.front());
				_TaskQueue.pop_front();
			}

			RUNTIME_TRACE_CALLBACK(UDatabaseManager, ExecuteTask, workerTrace.Get(), task.TraceSubmitSeq);
			ExecuteTask(connection, task);
		}
		connection.Close();
	}

	mysql_thread_end();
	if (_OnWorkerFinished)
		_OnWorkerFinished();
}

void UDatabaseManager::ExecuteTask(FMySqlConnection& connection, FDbTask& task) noexcept
{
	RUNTIME_TRACE_SCOPE(UDatabaseManager, ExecuteTask, GetRuntimeTraceID());
	try {
		if (auto* reg = std::get_if<FRegisterTask>(&task.Payload)) {
			FPasswordSalt salt{};
			FPasswordHash hash{};
			Core::FString detail;
			EDbRegisterStatus status = EDbRegisterStatus::DbError;
			if (!GeneratePasswordSalt(salt)
				|| !DerivePasswordHash(reg->Password.View(), salt, _Config.Pbkdf2Iterations, hash)) {
				detail = Core::FString{ "PBKDF2 hashing failed" };
			}
			else {
				status = connection.RegisterAccount(reg->Username.View(), reg->Nickname.View(), hash, salt, detail);
			}
			if (status == EDbRegisterStatus::DbError && !detail.empty())
				LogDbWarning("RegisterAccount", detail.View());
			if (reg->Callback)
				reg->Callback(status, std::move(detail));
			return;
		}

		if (auto* login = std::get_if<FLoginTask>(&task.Payload)) {
			FDbLoginOutcome outcome = connection.AuthenticateAndLogLogin(
				login->Username.View(), login->Password.View(), login->RemoteIp.View());
			if (outcome.Status == EDbLoginStatus::DbError && !outcome.ErrorDetail.empty())
				LogDbWarning("AuthenticateAndLogLogin", outcome.ErrorDetail.View());
			if (login->Callback)
				login->Callback(std::move(outcome));
			return;
		}

		if (auto* logout = std::get_if<FLogoutTask>(&task.Payload)) {
			Core::FString errorDetail;
			if (!connection.RecordLogout(logout->LogID, logout->LastRoomID, logout->Reason.View(), errorDetail))
				LogDbWarning("RecordLogout", errorDetail.View());
		}
	}
	catch (const std::exception& error) {
		LogDbWarning("ExecuteTask exception", error.what());
	}
	catch (...) {
		LogDbWarning("ExecuteTask exception", "unknown exception");
	}
}

void UDatabaseManager::LogDbWarning(const Core::STRINGVIEW where, const Core::STRINGVIEW detail) const noexcept
{
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (!logger)
		return;
	Core::FString message{ "UDatabaseManager " };
	message << where << ": " << detail;
	logger->TryLog(Core::ELogLevel::Warning, message.View());
}

SPACE_T
