#include "UAuthHandler.h"
#include "ChatProtocolLimits.h"
#include "UDatabaseManager.h"
#include "UPlayer.h"
#include "UCoreInstance.h"
#include "URoomManager.h"
#include "USession.h"

#include "packet_type.pb.h"
#include "login.pb.h"
#include "room.pb.h"

#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UAuthHandler.cpp"

namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UAuthHandler::UAuthHandler(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::SHPTR<Core::USession> session,
	Core::SHPTR<UNicknameRegistry> nicknameRegistry, Core::SHPTR<UDatabaseManager> databaseManager,
	Core::FString remoteIp)
	: UObject{ coreInstance },
	  UPacketHandlerBase{ std::move(coreInstance), std::move(session), "UAuthHandler" },
	  _NicknameRegistry{ std::move(nicknameRegistry) },
	  _DatabaseManager{ std::move(databaseManager) },
	  _RemoteIp{ std::move(remoteIp) }
{
	RUNTIME_TRACE_EVENT(Create, UAuthHandler, UAuthHandler, GetRuntimeTraceID());
}

UAuthHandler::~UAuthHandler()
{
	RUNTIME_TRACE_EVENT(Destroy, UAuthHandler, ~UAuthHandler, GetRuntimeTraceID());
}

// 연결 종료 시 진행 중인 로그인/가입 대기 상태를 해제한다.
void UAuthHandler::OnSessionDisconnected()
{
	RUNTIME_TRACE_SCOPE(UAuthHandler, OnSessionDisconnected, GetRuntimeTraceID());
	_Disconnected = true;
	_LoginPending = false;
	_RegisterPending = false;
}

// 미인증 상태에서 허용되는 인증 패킷만 처리하고, 미인증 도메인 패킷이나 미정의 번호는 즉시 차단한다.
void UAuthHandler::OnDomainPacket(const Core::FPacketView& packet)
{
	RUNTIME_TRACE_SCOPE(UAuthHandler, OnDomainPacket, GetRuntimeTraceID());
	switch (packet.GetType()) {
	case Protocol::CS_LOGIN:
		HandlePacket<Protocol::CSLogin>(packet, [this](const Protocol::CSLogin& parsed) { OnPacket(parsed); });
		break;
	case Protocol::CS_REGISTER:
		HandlePacket<Protocol::CSRegister>(packet, [this](const Protocol::CSRegister& parsed) { OnPacket(parsed); });
		break;
	case Protocol::CS_CHAT:
	case Protocol::CS_ROOM_USERS:
	case Protocol::CS_ROOM_LIST:
	case Protocol::CS_JOIN_ROOM:
		RejectPacket(packet.GetType(), "not logged in");
		break;
	default:
		RejectPacket(packet.GetType(), "undefined packet type");
		break;
	}
}

// 회원가입 요청을 검증한 뒤 DB 전용 스레드에 비동기 계정 생성을 위임한다.
void UAuthHandler::OnPacket(const Protocol::CSRegister& packet)
{
	RUNTIME_TRACE_SCOPE(UAuthHandler, OnPacket, GetRuntimeTraceID());
	Protocol::SCRegisterResult response;
	if (!_DatabaseManager || !_DatabaseManager->IsEnabled()) {
		response.set_result(Protocol::REGISTER_RESULT_DB_ERROR);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
		return;
	}
	if (_LoginPending || _RegisterPending) {
		response.set_result(Protocol::REGISTER_RESULT_NONE);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
		return;
	}

	const Core::STRINGVIEW username{ packet.username() };
	const Core::STRINGVIEW password{ packet.password() };
	const Core::STRINGVIEW nickname{ packet.nickname() };
	if (!IsValidUsername(username)) {
		response.set_result(Protocol::REGISTER_RESULT_INVALID_USERNAME);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
		return;
	}
	if (!IsValidPassword(password)) {
		response.set_result(Protocol::REGISTER_RESULT_INVALID_PASSWORD);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
		return;
	}
	if (!IsValidNickname(nickname)) {
		response.set_result(Protocol::REGISTER_RESULT_INVALID_NICKNAME);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
		return;
	}

	const auto session = GetBoundSession();
	if (!session)
		return;

	_RegisterPending = true;
	const Core::WKPTR<UAuthHandler> weakSelf = ThisWeak<UAuthHandler>();
	const Core::WKPTR<Core::USession> weakSession = session;
	const bool queued = _DatabaseManager->EnqueueRegister(
		Core::FString{ username }, Core::FString{ password }, Core::FString{ nickname },
		[weakSelf, weakSession](const EDbRegisterStatus status, Core::FString) {
			const auto self = weakSelf.lock();
			const auto boundSession = weakSession.lock();
			if (self && boundSession) {
				boundSession->PostToSession([self, status]() {
					self->OnDbRegisterCompleted(status);
				});
			}
		});
	if (!queued) {
		_RegisterPending = false;
		response.set_result(Protocol::REGISTER_RESULT_DB_ERROR);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
	}
}

// 로그인 요청을 처리한다(--db=mysql: DB 전용 스레드 PBKDF2 인증, --db=off: 인메모리 닉네임 인증).
void UAuthHandler::OnPacket(const Protocol::CSLogin& packet)
{
	RUNTIME_TRACE_SCOPE(UAuthHandler, OnPacket, GetRuntimeTraceID());
	Protocol::SCLoginResult response;
	if (_LoginPending) {
		response.set_result(Protocol::LOGIN_RESULT_ALREADY_LOGGED_IN);
		SendPacket(Protocol::SC_LOGIN_RESULT, response);
		return;
	}

	if (_DatabaseManager && _DatabaseManager->IsEnabled()) {
		const Core::STRINGVIEW username{ packet.username() };
		const Core::STRINGVIEW password{ packet.password() };
		if (!IsValidUsername(username) || !IsValidPassword(password)) {
			response.set_result(Protocol::LOGIN_RESULT_INVALID_CREDENTIALS);
			SendPacket(Protocol::SC_LOGIN_RESULT, response);
			return;
		}

		const auto session = GetBoundSession();
		if (!session)
			return;

		_LoginPending = true;
		const Core::WKPTR<UAuthHandler> weakSelf = ThisWeak<UAuthHandler>();
		const Core::WKPTR<Core::USession> weakSession = session;
		const auto dbManager = _DatabaseManager;
		const bool queued = dbManager->EnqueueLogin(
			Core::FString{ username }, Core::FString{ password }, _RemoteIp,
			[weakSelf, weakSession, dbManager](FDbLoginOutcome outcome) {
				const auto self = weakSelf.lock();
				const auto boundSession = weakSession.lock();
				if (self && boundSession) {
					boundSession->PostToSession([self, outcome = std::move(outcome)]() mutable {
						self->OnDbLoginCompleted(outcome);
					});
				}
				else if (outcome.Status == EDbLoginStatus::Success && outcome.ConnectionLogID != 0 && dbManager) {
					dbManager->EnqueueLogout(outcome.ConnectionLogID, static_cast<Core::uint32>(DEFAULT_GAME_ROOM_ID),
						Core::FString{ "ABORTED_LOGIN" });
				}
			});
		if (!queued) {
			_LoginPending = false;
			response.set_result(Protocol::LOGIN_RESULT_DB_ERROR);
			SendPacket(Protocol::SC_LOGIN_RESULT, response);
		}
		return;
	}

	// --db=off 인메모리 호환 모드: 닉네임 검증 후 즉시 기본 방 입장.
	const Core::STRINGVIEW nickname{ packet.nickname() };
	if (!IsValidNickname(nickname)) {
		response.set_result(Protocol::LOGIN_RESULT_INVALID_NICKNAME);
		SendPacket(Protocol::SC_LOGIN_RESULT, response);
		return;
	}

	BeginRoomEnter(Core::FString{ nickname }, 0, 0);
}

void UAuthHandler::OnDbRegisterCompleted(const EDbRegisterStatus status)
{
	RUNTIME_TRACE_SCOPE(UAuthHandler, OnDbRegisterCompleted, GetRuntimeTraceID());
	_RegisterPending = false;
	if (_Disconnected)
		return;

	Protocol::SCRegisterResult response;
	switch (status) {
	case EDbRegisterStatus::Success:
		response.set_result(Protocol::REGISTER_RESULT_SUCCESS);
		break;
	case EDbRegisterStatus::DuplicateUsername:
		response.set_result(Protocol::REGISTER_RESULT_DUPLICATE_USERNAME);
		break;
	case EDbRegisterStatus::DuplicateNickname:
		response.set_result(Protocol::REGISTER_RESULT_DUPLICATE_NICKNAME);
		break;
	default:
		response.set_result(Protocol::REGISTER_RESULT_DB_ERROR);
		break;
	}
	SendPacket(Protocol::SC_REGISTER_RESULT, response);
}

void UAuthHandler::OnDbLoginCompleted(const FDbLoginOutcome& outcome)
{
	RUNTIME_TRACE_SCOPE(UAuthHandler, OnDbLoginCompleted, GetRuntimeTraceID());
	if (_Disconnected) {
		_LoginPending = false;
		if (outcome.Status == EDbLoginStatus::Success && outcome.ConnectionLogID != 0 && _DatabaseManager) {
			_DatabaseManager->EnqueueLogout(outcome.ConnectionLogID, static_cast<Core::uint32>(DEFAULT_GAME_ROOM_ID),
				Core::FString{ "ABORTED_LOGIN" });
		}
		return;
	}

	if (outcome.Status != EDbLoginStatus::Success) {
		_LoginPending = false;
		Protocol::SCLoginResult response;
		response.set_result(outcome.Status == EDbLoginStatus::InvalidCredentials
			? Protocol::LOGIN_RESULT_INVALID_CREDENTIALS
			: Protocol::LOGIN_RESULT_DB_ERROR);
		SendPacket(Protocol::SC_LOGIN_RESULT, response);
		return;
	}

	BeginRoomEnter(outcome.Nickname, outcome.AccountID, outcome.ConnectionLogID);
}

void UAuthHandler::BeginRoomEnter(Core::FString nickname, const Core::uint64 accountId, const Core::uint64 connectionLogId)
{
	Protocol::SCLoginResult response;
	const auto coreInstance = GetCoreInstance();
	const auto session = GetBoundSession();
	const auto roomManager = coreInstance ? coreInstance->GetRoomManager() : nullptr;
	const auto gameRoom = roomManager ? roomManager->FindRoomAs<UGameRoom>(DEFAULT_GAME_ROOM_ID) : nullptr;
	if (!gameRoom || !session) {
		_LoginPending = false;
		if (connectionLogId != 0 && _DatabaseManager)
			_DatabaseManager->EnqueueLogout(connectionLogId, static_cast<Core::uint32>(DEFAULT_GAME_ROOM_ID), Core::FString{ "ROOM_NOT_FOUND" });
		RejectPacket(Protocol::CS_LOGIN, "default game room not found");
		return;
	}

	if (_NicknameRegistry && !_NicknameRegistry->TryReserve(nickname.View())) {
		_LoginPending = false;
		if (connectionLogId != 0 && _DatabaseManager)
			_DatabaseManager->EnqueueLogout(connectionLogId, static_cast<Core::uint32>(DEFAULT_GAME_ROOM_ID), Core::FString{ "ALREADY_ONLINE" });
		response.set_result(accountId != 0 ? Protocol::LOGIN_RESULT_ALREADY_ONLINE : Protocol::LOGIN_RESULT_DUPLICATE_NICKNAME);
		SendPacket(Protocol::SC_LOGIN_RESULT, response);
		return;
	}

	FPlayerInfo info{ std::move(nickname), accountId, connectionLogId };
	const auto player = Core::MakeShared<UPlayer>(coreInstance, session, std::move(info), _NicknameRegistry, _DatabaseManager);
	_LoginPending = true;

	const auto self = ThisShared<UAuthHandler>();
	const bool posted = gameRoom->RequestEnterPlayer(player,
		[self, session, gameRoom, player](UGameRoom::EEnterResult result, Core::uint32 userCount) {
			session->PostToSession([self, gameRoom, player, result, userCount] {
				self->OnEnterRoomCompleted(gameRoom, player, result, userCount);
			});
		});
	if (!posted) {
		_LoginPending = false;
		RejectPacket(Protocol::CS_LOGIN, "failed to post room enter");
	}
}

// 기본 방 입장 확정 시 USession 핸들러를 UPlayer로 원자 교체하고 응답을 송신한다.
void UAuthHandler::OnEnterRoomCompleted(const Core::SHPTR<UGameRoom>& gameRoom, const Core::SHPTR<UPlayer>& player,
	const UGameRoom::EEnterResult result, const Core::uint32 userCount)
{
	RUNTIME_TRACE_SCOPE(UAuthHandler, OnEnterRoomCompleted, GetRuntimeTraceID());
	_LoginPending = false;
	if (_Disconnected) {
		if (result == UGameRoom::EEnterResult::Success && gameRoom && player)
			gameRoom->RequestLeave(player);
		return;
	}

	if (result == UGameRoom::EEnterResult::InvalidActor)
		return;

	Protocol::SCLoginResult response;
	switch (result) {
	case UGameRoom::EEnterResult::Success: {
		const auto session = GetBoundSession();
		if (!session) {
			if (gameRoom && player)
				gameRoom->RequestLeave(player);
			return;
		}

		// SetHandler 호출로 USession의 기존 _Handler 참조가 해제되므로 함수 반환까지 수명을 스택에 고정한다.
		const auto keepAlive = ThisShared<UAuthHandler>();
		player->SetGameRoom(gameRoom);
		session->SetHandler(player);

		response.set_result(Protocol::LOGIN_RESULT_SUCCESS);
		response.set_user_count(userCount);
		response.set_room_id(static_cast<Core::uint32>(gameRoom->GetRoomID()));
		const Core::STRINGVIEW confirmedNick = player->GetInfo().Nickname.View();
		response.set_nickname(confirmedNick.data(), confirmedNick.size());
		SendPacket(Protocol::SC_LOGIN_RESULT, response);

		Protocol::SCRoomUserCount countPacket;
		countPacket.set_count(userCount);
		SendPacket(Protocol::SC_ROOM_USER_COUNT, countPacket);
		return;
	}
	case UGameRoom::EEnterResult::DuplicateNickname:
		response.set_result(Protocol::LOGIN_RESULT_DUPLICATE_NICKNAME);
		break;
	case UGameRoom::EEnterResult::RoomFull:
		response.set_result(Protocol::LOGIN_RESULT_ROOM_FULL);
		break;
	case UGameRoom::EEnterResult::AlreadyEntered:
		response.set_result(Protocol::LOGIN_RESULT_ALREADY_LOGGED_IN);
		break;
	default:
		response.set_result(Protocol::LOGIN_RESULT_NONE);
		break;
	}

	SendPacket(Protocol::SC_LOGIN_RESULT, response);
}

SPACE_T
