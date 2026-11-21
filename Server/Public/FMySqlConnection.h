#pragma once

#include "CoreDefine.h"
#include "PasswordHasher.h"

struct st_mysql;
using MYSQL = st_mysql;

SPACE_H(Server)

// MySQL 연결 및 DB 전용 스레드 풀 설정이다.
struct FDbConfig {
	bool Enabled{ true };
	Core::FString Host{ "127.0.0.1" };
	Core::uint16 Port{ 3306 };
	Core::FString User{ "multiserver" };
	Core::FString Password{ "multiserver_dev_pw" };
	Core::FString Database{ "multiserver" };
	Core::uint32 WorkerCount{ 4 };
	Core::uint32 Pbkdf2Iterations{ PASSWORD_PBKDF2_DEFAULT_ITERATIONS };
};

enum class EDbRegisterStatus : Core::uint8 {
	Success = 0,
	DuplicateUsername,
	DuplicateNickname,
	DbError,
};

enum class EDbLoginStatus : Core::uint8 {
	Success = 0,
	InvalidCredentials,
	DbError,
};

struct FDbLoginOutcome {
	EDbLoginStatus Status{ EDbLoginStatus::DbError };
	Core::uint64 AccountID{ 0 };
	Core::uint64 ConnectionLogID{ 0 };
	Core::FString Nickname;
	Core::FString ErrorDetail;
};

// DB 전용 스레드 1개가 전담 소유하는 RAII MySQL 연결 래퍼다.
// 모든 SQL은 Prepared Statement(? 바인딩)만 실행해 SQL 인젝션을 원천 차단한다.
class FMySqlConnection final {
public:
	explicit FMySqlConnection(FDbConfig config);
	~FMySqlConnection() noexcept;
	NO_COPY(FMySqlConnection)

	// 연결이 열려 있지 않거나 끊어졌으면 재연결을 시도한다.
	bool EnsureConnected(Core::FString& outError) noexcept;
	void Close() noexcept;

	// 신규 계정을 accounts 테이블에 원자적으로 삽입한다(UNIQUE 위반 1062로 중복 판정).
	EDbRegisterStatus RegisterAccount(Core::STRINGVIEW username, Core::STRINGVIEW nickname,
		const FPasswordHash& hash, const FPasswordSalt& salt, Core::FString& outError) noexcept;

	// 계정 조회 → PBKDF2 상수시간 검증 → last_login_at 갱신 + connection_logs 행 생성을 수행한다.
	FDbLoginOutcome AuthenticateAndLogLogin(Core::STRINGVIEW username, Core::STRINGVIEW password,
		Core::STRINGVIEW remoteIp) noexcept;

	// 종료된 접속의 logout_at, last_room_id, disconnect_reason을 connection_logs에 기록한다.
	bool RecordLogout(Core::uint64 logId, Core::uint32 lastRoomId,
		Core::STRINGVIEW disconnectReason, Core::FString& outError) noexcept;

private:
	bool ConnectInternal(Core::FString& outError) noexcept;

	FDbConfig _Config;
	MYSQL* _Handle{ nullptr };
};

SPACE_T
