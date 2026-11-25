#pragma once

#include "CoreDefine.h"
#include "FMySqlConnection.h"
#include "UObject.h"

#include <variant>

SPACE_H(Core)
class UThreadManager;
SPACE_T

SPACE_H(Server)

// DB 전용 스레드 풀 관리자다.
// 네트워크 워커와 URoom Strand에서 DB를 직접 호출하지 않고 이 관리자의 유계 작업 큐로 넘긴다.
// UThreadManager::RegisterFunc로 등록된 DB 전용 워커들이 각자 전용 FMySqlConnection 1개를 소유해
// 커넥션 풀 잠금 경합 없이 Prepared Statement를 실행하고, 종료 시 잔여 로그아웃 기록을 전부 배수한다.
class UDatabaseManager final : public Core::UObject {
public:
	STATIC_CONSTEXPR Core::uint64 DB_MAX_AUTH_PENDING_TASKS{ 4096 };
	STATIC_CONSTEXPR Core::uint64 DB_MAX_TOTAL_PENDING_TASKS{ 8192 };

	using FRegisterCallback = Core::FUNC<void(EDbRegisterStatus, Core::FString)>;
	using FLoginCallback = Core::FUNC<void(FDbLoginOutcome)>;
	using FWorkerFinishedCallback = Core::FUNC<void()>;

	explicit UDatabaseManager(Core::SHPTR<Core::UCoreInstance> coreInstance, FDbConfig config);
	virtual ~UDatabaseManager() override;
	NO_COPY(UDatabaseManager)

	// MySQL C 클라이언트 라이브러리 초기화 및 초기 DB 접속 확인을 수행한다(워커 시작 전 메인 스레드 호출).
	bool Initialize(Core::FString& outError);

	// UThreadManager::RegisterFunc에 DB 전용 스레드(_Config.WorkerCount개)를 등록한다.
	bool RegisterWorkers(const Core::SHPTR<Core::UThreadManager>& threadManager, FWorkerFinishedCallback onWorkerFinished);

	// 모든 네트워크 워커가 종료된 뒤 호출한다. 큐에 남은 로그아웃 기록(FLogoutTask)만 끝까지 배수하고
	// 이미 연결이 끊긴 가입·로그인 대기 작업은 건너뛴 뒤 DB 워커를 끝낸다.
	void RequestStop() noexcept;

	// 비동기 회원가입 요청을 DB 전용 큐에 적재한다(네트워크 스레드 비블로킹).
	bool EnqueueRegister(Core::FString username, Core::FString password, Core::FString nickname,
		FRegisterCallback callback);

	// 비동기 로그인 인증 + 접속 이력 생성을 DB 전용 큐에 적재한다.
	bool EnqueueLogin(Core::FString username, Core::FString password, Core::FString remoteIp,
		FLoginCallback callback);

	// 비동기 퇴장 이력(logout_at, last_room_id, disconnect_reason) 갱신을 큐에 적재한다.
	bool EnqueueLogout(Core::uint64 logId, Core::uint32 lastRoomId, Core::FString disconnectReason);

	bool IsEnabled() const noexcept { return _Config.Enabled; }
	const FDbConfig& GetConfig() const noexcept { return _Config; }

private:
	struct FRegisterTask {
		Core::FString Username;
		Core::FString Password;
		Core::FString Nickname;
		FRegisterCallback Callback;
	};

	struct FLoginTask {
		Core::FString Username;
		Core::FString Password;
		Core::FString RemoteIp;
		FLoginCallback Callback;
	};

	struct FLogoutTask {
		Core::uint64 LogID{ 0 };
		Core::uint32 LastRoomID{ 1 };
		Core::FString Reason;
	};

	using FTaskPayload = std::variant<FRegisterTask, FLoginTask, FLogoutTask>;

	struct FDbTask {
		FTaskPayload Payload;
		Core::uint64 TraceSubmitSeq{ 0 };
	};

	static void RunWorker(void* data) noexcept;
	void WorkerLoop() noexcept;
	void ExecuteTask(FMySqlConnection& connection, FDbTask& task) noexcept;
	void LogDbWarning(Core::STRINGVIEW where, Core::STRINGVIEW detail) const noexcept;

	FDbConfig _Config;
	bool _LibraryInitialized{ false };
	Core::SHARED_MUTEX _QueueMutex;
	Core::COND_VARIABLE _QueueCV;
	Core::DEQUE<FDbTask> _TaskQueue;
	bool _StopRequested{ false };
	FWorkerFinishedCallback _OnWorkerFinished;
};

SPACE_T
