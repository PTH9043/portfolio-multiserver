#include "FMySqlConnection.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <mysql.h>

#include <cstring>
#include <filesystem>
#include <string>

SPACE_H(Server)

namespace {
// MariaDB Connector/C 플러그인(caching_sha2_password 등) 경로를 실행 파일 옆 plugin 폴더에서 찾는다.
std::string ResolveMysqlPluginDir() noexcept
{
	try {
		char exePath[MAX_PATH]{};
		const DWORD len = GetModuleFileNameA(nullptr, exePath, MAX_PATH);
		if (len == 0 || len >= MAX_PATH)
			return {};
		const std::filesystem::path pluginDir = std::filesystem::path{ exePath }.parent_path() / "plugin";
		if (std::filesystem::is_directory(pluginDir))
			return pluginDir.string();
	}
	catch (...) {
	}
	return {};
}

// MYSQL_STMT RAII 래퍼다. 스코프를 벗어나면 자동으로 mysql_stmt_close를 호출한다.
class FMySqlStmtGuard final {
public:
	explicit FMySqlStmtGuard(MYSQL* conn) noexcept
		: _Stmt{ conn ? mysql_stmt_init(conn) : nullptr }
	{}

	~FMySqlStmtGuard() noexcept
	{
		if (_Stmt != nullptr)
			mysql_stmt_close(_Stmt);
	}

	NO_COPY(FMySqlStmtGuard)

	MYSQL_STMT* Get() const noexcept { return _Stmt; }
	explicit operator bool() const noexcept { return _Stmt != nullptr; }

private:
	MYSQL_STMT* _Stmt{ nullptr };
};
}

FMySqlConnection::FMySqlConnection(FDbConfig config)
	: _Config{ std::move(config) }
{
}

FMySqlConnection::~FMySqlConnection() noexcept
{
	Close();
}

bool FMySqlConnection::EnsureConnected(Core::FString& outError) noexcept
{
	if (_Handle != nullptr) {
		if (mysql_ping(_Handle) == 0)
			return true;
		Close();
	}
	return ConnectInternal(outError);
}

void FMySqlConnection::Close() noexcept
{
	if (_Handle != nullptr) {
		mysql_close(_Handle);
		_Handle = nullptr;
	}
}

EDbRegisterStatus FMySqlConnection::RegisterAccount(const Core::STRINGVIEW username, const Core::STRINGVIEW nickname,
	const FPasswordHash& hash, const FPasswordSalt& salt, Core::FString& outError) noexcept
{
	if (!EnsureConnected(outError))
		return EDbRegisterStatus::DbError;

	constexpr const char* SQL =
		"INSERT INTO accounts (username, nickname, password_hash, password_salt) VALUES (?, ?, ?, ?)";
	FMySqlStmtGuard stmt{ _Handle };
	if (!stmt || mysql_stmt_prepare(stmt.Get(), SQL, static_cast<unsigned long>(std::strlen(SQL))) != 0) {
		outError = Core::FString{ mysql_error(_Handle) };
		return EDbRegisterStatus::DbError;
	}

	MYSQL_BIND binds[4]{};
	unsigned long userLen = static_cast<unsigned long>(username.size());
	unsigned long nickLen = static_cast<unsigned long>(nickname.size());
	unsigned long hashLen = static_cast<unsigned long>(hash.size());
	unsigned long saltLen = static_cast<unsigned long>(salt.size());

	binds[0].buffer_type = MYSQL_TYPE_STRING;
	binds[0].buffer = const_cast<char*>(username.data());
	binds[0].buffer_length = userLen;
	binds[0].length = &userLen;

	binds[1].buffer_type = MYSQL_TYPE_STRING;
	binds[1].buffer = const_cast<char*>(nickname.data());
	binds[1].buffer_length = nickLen;
	binds[1].length = &nickLen;

	binds[2].buffer_type = MYSQL_TYPE_BLOB;
	binds[2].buffer = const_cast<Core::uint8*>(hash.data());
	binds[2].buffer_length = hashLen;
	binds[2].length = &hashLen;

	binds[3].buffer_type = MYSQL_TYPE_BLOB;
	binds[3].buffer = const_cast<Core::uint8*>(salt.data());
	binds[3].buffer_length = saltLen;
	binds[3].length = &saltLen;

	if (mysql_stmt_bind_param(stmt.Get(), binds) != 0 || mysql_stmt_execute(stmt.Get()) != 0) {
		const unsigned int errNo = mysql_stmt_errno(stmt.Get());
		const char* errMsg = mysql_stmt_error(stmt.Get());
		const Core::STRINGVIEW msgView{ errMsg ? errMsg : "" };
		// 1062 = ER_DUP_ENTRY: UNIQUE 제약으로 동시 가입 경합을 원자 판정한다.
		if (errNo == 1062u) {
			if (msgView.find("uq_accounts_nickname") != Core::STRINGVIEW::npos
				|| msgView.find("nickname") != Core::STRINGVIEW::npos)
				return EDbRegisterStatus::DuplicateNickname;
			return EDbRegisterStatus::DuplicateUsername;
		}
		outError = Core::FString{ msgView };
		if (errNo == 2006u || errNo == 2013u)
			Close();
		return EDbRegisterStatus::DbError;
	}

	return EDbRegisterStatus::Success;
}

FDbLoginOutcome FMySqlConnection::AuthenticateAndLogLogin(const Core::STRINGVIEW username,
	const Core::STRINGVIEW password, const Core::STRINGVIEW remoteIp) noexcept
{
	FDbLoginOutcome outcome;
	if (!EnsureConnected(outcome.ErrorDetail)) {
		outcome.Status = EDbLoginStatus::DbError;
		return outcome;
	}

	// 1) 계정 조회 (Prepared Statement)
	constexpr const char* SELECT_SQL =
		"SELECT account_id, nickname, password_hash, password_salt FROM accounts WHERE username = ? LIMIT 1";
	Core::uint64 accountId{ 0 };
	char nickBuf[64]{};
	unsigned long nickLen{ 0 };
	FPasswordHash storedHash{};
	unsigned long hashLen{ 0 };
	FPasswordSalt storedSalt{};
	unsigned long saltLen{ 0 };
	my_bool isNull[4]{};

	{
		FMySqlStmtGuard stmt{ _Handle };
		if (!stmt || mysql_stmt_prepare(stmt.Get(), SELECT_SQL, static_cast<unsigned long>(std::strlen(SELECT_SQL))) != 0) {
			outcome.ErrorDetail = Core::FString{ mysql_error(_Handle) };
			outcome.Status = EDbLoginStatus::DbError;
			return outcome;
		}

		MYSQL_BIND inBind{};
		unsigned long userLen = static_cast<unsigned long>(username.size());
		inBind.buffer_type = MYSQL_TYPE_STRING;
		inBind.buffer = const_cast<char*>(username.data());
		inBind.buffer_length = userLen;
		inBind.length = &userLen;

		if (mysql_stmt_bind_param(stmt.Get(), &inBind) != 0 || mysql_stmt_execute(stmt.Get()) != 0) {
			const unsigned int errNo = mysql_stmt_errno(stmt.Get());
			outcome.ErrorDetail = Core::FString{ mysql_stmt_error(stmt.Get()) };
			if (errNo == 2006u || errNo == 2013u)
				Close();
			outcome.Status = EDbLoginStatus::DbError;
			return outcome;
		}

		MYSQL_BIND outBinds[4]{};
		outBinds[0].buffer_type = MYSQL_TYPE_LONGLONG;
		outBinds[0].is_unsigned = 1;
		outBinds[0].buffer = &accountId;
		outBinds[0].is_null = &isNull[0];

		outBinds[1].buffer_type = MYSQL_TYPE_STRING;
		outBinds[1].buffer = nickBuf;
		outBinds[1].buffer_length = static_cast<unsigned long>(sizeof(nickBuf) - 1);
		outBinds[1].length = &nickLen;
		outBinds[1].is_null = &isNull[1];

		outBinds[2].buffer_type = MYSQL_TYPE_BLOB;
		outBinds[2].buffer = storedHash.data();
		outBinds[2].buffer_length = static_cast<unsigned long>(storedHash.size());
		outBinds[2].length = &hashLen;
		outBinds[2].is_null = &isNull[2];

		outBinds[3].buffer_type = MYSQL_TYPE_BLOB;
		outBinds[3].buffer = storedSalt.data();
		outBinds[3].buffer_length = static_cast<unsigned long>(storedSalt.size());
		outBinds[3].length = &saltLen;
		outBinds[3].is_null = &isNull[3];

		if (mysql_stmt_bind_result(stmt.Get(), outBinds) != 0 || mysql_stmt_store_result(stmt.Get()) != 0) {
			outcome.ErrorDetail = Core::FString{ mysql_stmt_error(stmt.Get()) };
			outcome.Status = EDbLoginStatus::DbError;
			return outcome;
		}

		const int fetchRc = mysql_stmt_fetch(stmt.Get());
		if (fetchRc == MYSQL_NO_DATA) {
			outcome.Status = EDbLoginStatus::InvalidCredentials;
			return outcome;
		}
		if (fetchRc != 0 && fetchRc != MYSQL_DATA_TRUNCATED) {
			outcome.ErrorDetail = Core::FString{ mysql_stmt_error(stmt.Get()) };
			outcome.Status = EDbLoginStatus::DbError;
			return outcome;
		}
	}

	if (isNull[0] || isNull[1] || isNull[2] || isNull[3]
		|| hashLen != PASSWORD_HASH_BYTES || saltLen != PASSWORD_SALT_BYTES) {
		outcome.Status = EDbLoginStatus::InvalidCredentials;
		return outcome;
	}

	// 2) PBKDF2-HMAC-SHA256 상수시간 검증
	if (!VerifyPasswordHash(password, storedSalt, storedHash, _Config.Pbkdf2Iterations)) {
		outcome.Status = EDbLoginStatus::InvalidCredentials;
		return outcome;
	}

	// 3) 최근 로그인 시각 갱신
	{
		constexpr const char* UPDATE_SQL = "UPDATE accounts SET last_login_at = NOW(3) WHERE account_id = ?";
		FMySqlStmtGuard stmt{ _Handle };
		if (stmt && mysql_stmt_prepare(stmt.Get(), UPDATE_SQL, static_cast<unsigned long>(std::strlen(UPDATE_SQL))) == 0) {
			MYSQL_BIND bind{};
			bind.buffer_type = MYSQL_TYPE_LONGLONG;
			bind.is_unsigned = 1;
			bind.buffer = &accountId;
			if (mysql_stmt_bind_param(stmt.Get(), &bind) == 0)
				(void)mysql_stmt_execute(stmt.Get());
		}
	}

	// 4) 접속 이력 생성(connection_logs)
	Core::uint64 logId{ 0 };
	{
		constexpr const char* INSERT_LOG_SQL =
			"INSERT INTO connection_logs (account_id, remote_ip, last_room_id, disconnect_reason) VALUES (?, ?, 1, 'CONNECTED')";
		FMySqlStmtGuard stmt{ _Handle };
		if (!stmt || mysql_stmt_prepare(stmt.Get(), INSERT_LOG_SQL, static_cast<unsigned long>(std::strlen(INSERT_LOG_SQL))) != 0) {
			outcome.ErrorDetail = Core::FString{ mysql_error(_Handle) };
			outcome.Status = EDbLoginStatus::DbError;
			return outcome;
		}

		const Core::STRINGVIEW ipView = remoteIp.empty() ? Core::STRINGVIEW{ "127.0.0.1" } : remoteIp;
		unsigned long ipLen = static_cast<unsigned long>(ipView.size() > 45 ? 45 : ipView.size());
		MYSQL_BIND binds[2]{};
		binds[0].buffer_type = MYSQL_TYPE_LONGLONG;
		binds[0].is_unsigned = 1;
		binds[0].buffer = &accountId;

		binds[1].buffer_type = MYSQL_TYPE_STRING;
		binds[1].buffer = const_cast<char*>(ipView.data());
		binds[1].buffer_length = ipLen;
		binds[1].length = &ipLen;

		if (mysql_stmt_bind_param(stmt.Get(), binds) != 0 || mysql_stmt_execute(stmt.Get()) != 0) {
			outcome.ErrorDetail = Core::FString{ mysql_stmt_error(stmt.Get()) };
			outcome.Status = EDbLoginStatus::DbError;
			return outcome;
		}
		logId = static_cast<Core::uint64>(mysql_stmt_insert_id(stmt.Get()));
	}

	const Core::uint64 safeNickLen = nickLen < sizeof(nickBuf) ? static_cast<Core::uint64>(nickLen) : static_cast<Core::uint64>(sizeof(nickBuf) - 1);
	outcome.Status = EDbLoginStatus::Success;
	outcome.AccountID = accountId;
	outcome.ConnectionLogID = logId;
	outcome.Nickname = Core::FString{ Core::STRINGVIEW{ nickBuf, safeNickLen } };
	return outcome;
}

bool FMySqlConnection::RecordLogout(const Core::uint64 logId, const Core::uint32 lastRoomId,
	const Core::STRINGVIEW disconnectReason, Core::FString& outError) noexcept
{
	if (logId == 0)
		return true;
	if (!EnsureConnected(outError))
		return false;

	constexpr const char* SQL =
		"UPDATE connection_logs SET logout_at = NOW(3), last_room_id = ?, disconnect_reason = ? WHERE log_id = ?";
	FMySqlStmtGuard stmt{ _Handle };
	if (!stmt || mysql_stmt_prepare(stmt.Get(), SQL, static_cast<unsigned long>(std::strlen(SQL))) != 0) {
		outError = Core::FString{ mysql_error(_Handle) };
		return false;
	}

	Core::uint32 roomVal = lastRoomId;
	Core::uint64 idVal = logId;
	const Core::STRINGVIEW reasonView = disconnectReason.empty() ? Core::STRINGVIEW{ "NORMAL" } : disconnectReason;
	unsigned long reasonLen = static_cast<unsigned long>(reasonView.size() > 32 ? 32 : reasonView.size());

	MYSQL_BIND binds[3]{};
	binds[0].buffer_type = MYSQL_TYPE_LONG;
	binds[0].is_unsigned = 1;
	binds[0].buffer = &roomVal;

	binds[1].buffer_type = MYSQL_TYPE_STRING;
	binds[1].buffer = const_cast<char*>(reasonView.data());
	binds[1].buffer_length = reasonLen;
	binds[1].length = &reasonLen;

	binds[2].buffer_type = MYSQL_TYPE_LONGLONG;
	binds[2].is_unsigned = 1;
	binds[2].buffer = &idVal;

	if (mysql_stmt_bind_param(stmt.Get(), binds) != 0 || mysql_stmt_execute(stmt.Get()) != 0) {
		const unsigned int errNo = mysql_stmt_errno(stmt.Get());
		outError = Core::FString{ mysql_stmt_error(stmt.Get()) };
		if (errNo == 2006u || errNo == 2013u)
			Close();
		return false;
	}

	return true;
}

bool FMySqlConnection::ConnectInternal(Core::FString& outError) noexcept
{
	Close();
	_Handle = mysql_init(nullptr);
	if (_Handle == nullptr) {
		outError = Core::FString{ "mysql_init failed" };
		return false;
	}

	const unsigned int timeoutSec{ 5 };
	const my_bool verifyCert{ 0 };
	const my_bool enforceSsl{ 0 };
	mysql_optionsv(_Handle, MYSQL_OPT_CONNECT_TIMEOUT, &timeoutSec);
	mysql_optionsv(_Handle, MYSQL_OPT_READ_TIMEOUT, &timeoutSec);
	mysql_optionsv(_Handle, MYSQL_OPT_WRITE_TIMEOUT, &timeoutSec);
	mysql_optionsv(_Handle, MYSQL_SET_CHARSET_NAME, "utf8mb4");
	mysql_optionsv(_Handle, MYSQL_OPT_SSL_VERIFY_SERVER_CERT, &verifyCert);
	mysql_optionsv(_Handle, MYSQL_OPT_SSL_ENFORCE, &enforceSsl);

	const std::string pluginDir = ResolveMysqlPluginDir();
	if (!pluginDir.empty())
		mysql_optionsv(_Handle, MYSQL_PLUGIN_DIR, pluginDir.c_str());

	const std::string host{ _Config.Host.View() };
	const std::string user{ _Config.User.View() };
	const std::string pass{ _Config.Password.View() };
	const std::string db{ _Config.Database.View() };

	if (mysql_real_connect(_Handle, host.c_str(), user.c_str(), pass.c_str(),
		db.c_str(), _Config.Port, nullptr, 0) == nullptr) {
		const char* errText = mysql_error(_Handle);
		outError = Core::FString{ errText ? errText : "mysql_real_connect failed" };
		Close();
		return false;
	}

	return true;
}

SPACE_T
