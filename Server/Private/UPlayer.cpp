#include "UPlayer.h"
#include "ChatProtocolLimits.h"
#include "UDatabaseManager.h"
#include "UGameRoom.h"
#include "USession.h"
#include "UCoreInstance.h"
#include "URoomManager.h"
#include "ULogger.h"

#include "packet_type.pb.h"
#include "login.pb.h"
#include "chat.pb.h"
#include "room.pb.h"

#include <string>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UPlayer.cpp"

namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UPlayer::UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance,
	Core::SHPTR<UNicknameRegistry> nicknameRegistry,
	Core::SHPTR<UDatabaseManager> databaseManager,
	Core::FString remoteIp,
	FDisconnectCleanupCallback onDisconnectedCleanup)
	: UPawn{ std::move(coreInstance) },
	  _NicknameRegistry{ std::move(nicknameRegistry) },
	  _DatabaseManager{ std::move(databaseManager) },
	  _RemoteIp{ std::move(remoteIp) },
	  _OnDisconnectedCleanup{ std::move(onDisconnectedCleanup) }
{
	RUNTIME_TRACE_EVENT(Create, UPlayer, UPlayer, GetRuntimeTraceID());
}

UPlayer::~UPlayer()
{
	ReleaseNicknameOnce();
	if (_DatabaseManager && _Info.ConnectionLogID != 0
		&& !_LogoutRecorded.exchange(true, std::memory_order_acq_rel)) {
		_DatabaseManager->EnqueueLogout(_Info.ConnectionLogID, _LastRoomID, Core::FString{ "CLEANUP" });
	}
	RUNTIME_TRACE_EVENT(Destroy, UPlayer, ~UPlayer, GetRuntimeTraceID());
}

// 세션 종료 시 전역 닉네임 해제 · 소속 방 퇴장 · DB 로그아웃 · 리스너 활성 목록 제거를 수행한다.
void UPlayer::OnSessionDisconnected()
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnSessionDisconnected, GetRuntimeTraceID());
	const auto keepAlive = ThisShared<UPlayer>();
	_Disconnected = true;
	_LoginPending = false;
	_RegisterPending = false;
	_RoomTransferPending = false;
	ReleaseNicknameOnce();

	const auto gameRoom = _GameRoom.lock();
	if (gameRoom)
		_LastRoomID = static_cast<Core::uint32>(gameRoom->GetRoomID());
	_GameRoom.reset();
	if (gameRoom)
		gameRoom->RequestLeave(keepAlive);

	if (_DatabaseManager && _Info.ConnectionLogID != 0
		&& !_LogoutRecorded.exchange(true, std::memory_order_acq_rel)) {
		_DatabaseManager->EnqueueLogout(_Info.ConnectionLogID, _LastRoomID, _DisconnectReason);
	}

	if (FDisconnectCleanupCallback cleanup = std::move(_OnDisconnectedCleanup))
		cleanup(keepAlive);
}

// UPawn이 공용 패킷(CS_PING)을 선처리한 뒤 위임하는 도메인 패킷 분기점이다.
void UPlayer::OnDomainPacket(const Core::FPacketView& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnDomainPacket, GetRuntimeTraceID());
	switch (packet.GetType()) {
	case Protocol::CS_REGISTER:
		HandlePacket<Protocol::CSRegister>(packet, [this](const Protocol::CSRegister& parsed) { OnPacket(parsed); });
		break;
	case Protocol::CS_LOGIN:
		HandlePacket<Protocol::CSLogin>(packet, [this](const Protocol::CSLogin& parsed) { OnPacket(parsed); });
		break;
	case Protocol::CS_CHAT:
		if (!_LoggedIn) {
			RejectPacket(packet.GetType(), "not logged in");
			return;
		}
		HandlePacket<Protocol::CSChat>(packet, [this](const Protocol::CSChat& parsed) { OnPacket(parsed); });
		break;
	case Protocol::CS_ROOM_USERS:
		if (!_LoggedIn) {
			RejectPacket(packet.GetType(), "not logged in");
			return;
		}
		HandlePacket<Protocol::CSRoomUsers>(packet, [this](const Protocol::CSRoomUsers& parsed) { OnPacket(parsed); });
		break;
	case Protocol::CS_ROOM_LIST:
		if (!_LoggedIn) {
			RejectPacket(packet.GetType(), "not logged in");
			return;
		}
		HandlePacket<Protocol::CSRoomList>(packet, [this](const Protocol::CSRoomList& parsed) { OnPacket(parsed); });
		break;
	case Protocol::CS_JOIN_ROOM:
		if (!_LoggedIn) {
			RejectPacket(packet.GetType(), "not logged in");
			return;
		}
		HandlePacket<Protocol::CSJoinRoom>(packet, [this](const Protocol::CSJoinRoom& parsed) { OnPacket(parsed); });
		break;
	default:
		RejectPacket(packet.GetType(), "undefined packet type");
		break;
	}
}

void UPlayer::OnPacketRejected() noexcept
{
	_DisconnectReason = Core::FString{ "PROTOCOL_ERROR" };
}

// 회원가입 요청을 검증한 뒤 DB 전용 스레드에 비동기 계정 생성을 위임한다.
void UPlayer::OnPacket(const Protocol::CSRegister& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	Protocol::SCRegisterResult response;
	if (!_DatabaseManager || !_DatabaseManager->IsEnabled()) {
		response.set_result(Protocol::REGISTER_RESULT_DB_ERROR);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
		return;
	}
	if (_LoggedIn || _LoginPending || _RegisterPending) {
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

	const auto session = GetSession();
	if (!session)
		return;

	_RegisterPending = true;
	const Core::WKPTR<UPlayer> weakSelf = ThisWeak<UPlayer>();
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
void UPlayer::OnPacket(const Protocol::CSLogin& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	Protocol::SCLoginResult response;
	if (_LoggedIn || _LoginPending) {
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

		const auto session = GetSession();
		if (!session)
			return;

		_LoginPending = true;
		const Core::WKPTR<UPlayer> weakSelf = ThisWeak<UPlayer>();
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

	BeginInitialRoomEnter(Core::FString{ nickname }, 0, 0);
}

void UPlayer::OnDbRegisterCompleted(const EDbRegisterStatus status)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnDbRegisterCompleted, GetRuntimeTraceID());
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

void UPlayer::OnDbLoginCompleted(const FDbLoginOutcome& outcome)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnDbLoginCompleted, GetRuntimeTraceID());
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

	BeginInitialRoomEnter(outcome.Nickname, outcome.AccountID, outcome.ConnectionLogID);
}

void UPlayer::BeginInitialRoomEnter(Core::FString nickname, const Core::uint64 accountId, const Core::uint64 connectionLogId)
{
	Protocol::SCLoginResult response;
	const auto coreInstance = GetCoreInstance();
	const auto session = GetSession();
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

	_Info = FPlayerInfo{ std::move(nickname), accountId, connectionLogId };
	_NicknameReserved.store(true, std::memory_order_release);
	_LogoutRecorded.store(false, std::memory_order_release);
	_LoginPending = true;

	const auto self = ThisShared<UPlayer>();
	const bool posted = gameRoom->RequestEnterPlayer(self,
		[self, session, gameRoom](UGameRoom::EEnterResult result, Core::uint32 userCount) {
			session->PostToSession([self, gameRoom, result, userCount] {
				self->OnEnterInitialRoomCompleted(gameRoom, result, userCount);
			});
		});
	if (!posted) {
		_LoginPending = false;
		ReleaseNicknameOnce();
		if (connectionLogId != 0 && _DatabaseManager
			&& !_LogoutRecorded.exchange(true, std::memory_order_acq_rel)) {
			_DatabaseManager->EnqueueLogout(connectionLogId, static_cast<Core::uint32>(DEFAULT_GAME_ROOM_ID), Core::FString{ "ROOM_POST_FAIL" });
		}
		_Info = FPlayerInfo{};
		RejectPacket(Protocol::CS_LOGIN, "failed to post room enter");
	}
}

void UPlayer::OnEnterInitialRoomCompleted(const Core::SHPTR<UGameRoom>& gameRoom,
	const UGameRoom::EEnterResult result, const Core::uint32 userCount)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnEnterInitialRoomCompleted, GetRuntimeTraceID());
	_LoginPending = false;
	if (_Disconnected) {
		if (result == UGameRoom::EEnterResult::Success && gameRoom)
			gameRoom->RequestLeave(ThisShared<UPlayer>());
		return;
	}

	if (result == UGameRoom::EEnterResult::InvalidActor)
		return;

	Protocol::SCLoginResult response;
	if (result == UGameRoom::EEnterResult::Success && gameRoom) {
		_LoggedIn = true;
		_GameRoom = gameRoom;
		_LastRoomID = static_cast<Core::uint32>(gameRoom->GetRoomID());

		response.set_result(Protocol::LOGIN_RESULT_SUCCESS);
		response.set_user_count(userCount);
		response.set_room_id(static_cast<Core::uint32>(gameRoom->GetRoomID()));
		const Core::STRINGVIEW confirmedNick = _Info.Nickname.View();
		response.set_nickname(confirmedNick.data(), confirmedNick.size());
		SendPacket(Protocol::SC_LOGIN_RESULT, response);

		Protocol::SCRoomUserCount countPacket;
		countPacket.set_count(userCount);
		SendPacket(Protocol::SC_ROOM_USER_COUNT, countPacket);
		return;
	}

	// 입장 실패 시 예약 닉네임과 접속 이력을 즉시 정리해 동일 연결 재시도를 허용한다.
	ReleaseNicknameOnce();
	if (_DatabaseManager && _Info.ConnectionLogID != 0
		&& !_LogoutRecorded.exchange(true, std::memory_order_acq_rel)) {
		_DatabaseManager->EnqueueLogout(_Info.ConnectionLogID, static_cast<Core::uint32>(DEFAULT_GAME_ROOM_ID), Core::FString{ "ENTER_REJECTED" });
	}
	_Info = FPlayerInfo{};

	switch (result) {
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

// 채팅 문자열을 검증한 뒤 소속 방 전체에 브로드캐스트를 요청한다.
void UPlayer::OnPacket(const Protocol::CSChat& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	const auto gameRoom = _GameRoom.lock();
	if (!gameRoom) {
		RejectPacket(Protocol::CS_CHAT, "not logged in");
		return;
	}

	const Core::STRINGVIEW text{ packet.text() };
	const EChatTextError textError = ValidateChatText(text);
	if (textError != EChatTextError::None) {
		const auto coreInstance = GetCoreInstance();
		const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
		if (logger) {
			Core::FString message{ "UPlayer ignored invalid chat text (" };
			message << GetChatTextErrorReason(textError) << ", " << std::to_string(text.size()) << " bytes)";
			logger->TryLog(Core::ELogLevel::Warning, message.View());
		}
		return;
	}

	gameRoom->RequestBroadcastChat(ThisShared<UPlayer>(), Core::FString{ text });
}

// 현재 방 접속자 목록 페이징 조회를 방에 요청한다.
void UPlayer::OnPacket(const Protocol::CSRoomUsers& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	const auto gameRoom = _GameRoom.lock();
	if (!gameRoom) {
		RejectPacket(Protocol::CS_ROOM_USERS, "not logged in");
		return;
	}

	gameRoom->RequestRoomUsers(ThisShared<UPlayer>(), packet.offset());
}

// 등록된 고정 채널/방 목록과 무잠금 인원 수 스냅샷을 응답한다.
void UPlayer::OnPacket(const Protocol::CSRoomList& packet)
{
	(void)packet;
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto roomManager = coreInstance ? coreInstance->GetRoomManager() : nullptr;
	if (!roomManager)
		return;

	Protocol::SCRoomList response;
	const auto rooms = roomManager->SnapshotRooms();
	for (const auto& baseRoom : rooms) {
		if (!baseRoom || baseRoom->GetRoomTypeID() != UGameRoom::RoomTypeID)
			continue;
		const auto* gameRoom = static_cast<const UGameRoom*>(baseRoom.get());
		auto* summary = response.add_rooms();
		summary->set_room_id(static_cast<Core::uint32>(gameRoom->GetRoomID()));
		const Core::STRINGVIEW roomName{ gameRoom->GetRoomName().View() };
		summary->set_name(roomName.data(), roomName.size());
		summary->set_user_count(static_cast<Core::uint32>(gameRoom->GetApproximateActorCount()));
		summary->set_max_users(gameRoom->GetMaxActorCount());
	}

	SendPacket(Protocol::SC_ROOM_LIST, response);
}

// 현재 방에서 안전하게 퇴장한 뒤 대상 방으로 크로스-스트랜드 이동을 시작한다.
void UPlayer::OnPacket(const Protocol::CSJoinRoom& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	Protocol::SCJoinRoomResult response;
	const auto currentRoom = _GameRoom.lock();
	if (!currentRoom) {
		RejectPacket(Protocol::CS_JOIN_ROOM, "not logged in");
		return;
	}
	if (_RoomTransferPending) {
		response.set_result(Protocol::JOIN_ROOM_TRANSFER_IN_PROGRESS);
		response.set_room_id(static_cast<Core::uint32>(currentRoom->GetRoomID()));
		SendPacket(Protocol::SC_JOIN_ROOM_RESULT, response);
		return;
	}

	const Core::uint64 targetRoomID{ static_cast<Core::uint64>(packet.room_id()) };
	if (targetRoomID == currentRoom->GetRoomID()) {
		response.set_result(Protocol::JOIN_ROOM_ALREADY_IN_ROOM);
		response.set_room_id(static_cast<Core::uint32>(currentRoom->GetRoomID()));
		response.set_user_count(static_cast<Core::uint32>(currentRoom->GetApproximateActorCount()));
		SendPacket(Protocol::SC_JOIN_ROOM_RESULT, response);
		return;
	}

	const auto coreInstance = GetCoreInstance();
	const auto roomManager = coreInstance ? coreInstance->GetRoomManager() : nullptr;
	const auto targetRoom = roomManager ? roomManager->FindRoomAs<UGameRoom>(targetRoomID) : nullptr;
	if (!targetRoom) {
		response.set_result(Protocol::JOIN_ROOM_NOT_FOUND);
		response.set_room_id(static_cast<Core::uint32>(currentRoom->GetRoomID()));
		SendPacket(Protocol::SC_JOIN_ROOM_RESULT, response);
		return;
	}

	const auto session = GetSession();
	if (!session)
		return;

	_RoomTransferPending = true;
	const auto self = ThisShared<UPlayer>();
	const bool posted = currentRoom->RequestLeaveWithCallback(self,
		[self, session, currentRoom, targetRoom](bool) {
			session->PostToSession([self, currentRoom, targetRoom] {
				self->OnLeaveRoomForTransferCompleted(currentRoom, targetRoom);
			});
		});
	if (!posted) {
		_RoomTransferPending = false;
		response.set_result(Protocol::JOIN_ROOM_NONE);
		response.set_room_id(static_cast<Core::uint32>(currentRoom->GetRoomID()));
		SendPacket(Protocol::SC_JOIN_ROOM_RESULT, response);
	}
}

// 이전 방 퇴장 완료 후 대상 방 입장을 요청한다(중도 이탈 시 즉시 중단).
void UPlayer::OnLeaveRoomForTransferCompleted(const Core::SHPTR<UGameRoom>& previousRoom, const Core::SHPTR<UGameRoom>& targetRoom)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnLeaveRoomForTransferCompleted, GetRuntimeTraceID());
	if (_Disconnected) {
		_RoomTransferPending = false;
		return;
	}

	const auto session = GetSession();
	if (!session) {
		_RoomTransferPending = false;
		return;
	}

	const auto self = ThisShared<UPlayer>();
	const bool posted = targetRoom->RequestEnterPlayer(self,
		[self, session, previousRoom, targetRoom](UGameRoom::EEnterResult result, Core::uint32 userCount) {
			session->PostToSession([self, previousRoom, targetRoom, result, userCount] {
				self->OnEnterTransferTargetCompleted(previousRoom, targetRoom, result, userCount);
			});
		});
	if (!posted) {
		_RoomTransferPending = false;
		RejectPacket(Protocol::CS_JOIN_ROOM, "failed to post target room enter");
	}
}

// 대상 방 입장 결과를 반영하고, 만실 등 실패 시 이전 방으로 안전하게 복귀한다.
void UPlayer::OnEnterTransferTargetCompleted(const Core::SHPTR<UGameRoom>& previousRoom, const Core::SHPTR<UGameRoom>& targetRoom,
	const UGameRoom::EEnterResult result, const Core::uint32 userCount)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnEnterTransferTargetCompleted, GetRuntimeTraceID());
	if (_Disconnected) {
		_RoomTransferPending = false;
		if (result == UGameRoom::EEnterResult::Success && targetRoom)
			targetRoom->RequestLeave(ThisShared<UPlayer>());
		return;
	}

	if (result == UGameRoom::EEnterResult::Success) {
		_RoomTransferPending = false;
		_GameRoom = targetRoom;
		_LastRoomID = static_cast<Core::uint32>(targetRoom->GetRoomID());

		Protocol::SCJoinRoomResult response;
		response.set_result(Protocol::JOIN_ROOM_SUCCESS);
		response.set_room_id(static_cast<Core::uint32>(targetRoom->GetRoomID()));
		response.set_user_count(userCount);
		SendPacket(Protocol::SC_JOIN_ROOM_RESULT, response);

		Protocol::SCRoomUserCount countPacket;
		countPacket.set_count(userCount);
		SendPacket(Protocol::SC_ROOM_USER_COUNT, countPacket);
		return;
	}

	// 대상 방 입장이 거절되면 플레이어가 고아 상태가 되지 않도록 이전 방 복귀를 시도한다.
	const auto session = GetSession();
	if (previousRoom && session) {
		const auto self = ThisShared<UPlayer>();
		const bool posted = previousRoom->RequestEnterPlayer(self,
			[self, session, previousRoom](UGameRoom::EEnterResult rollbackResult, Core::uint32 rollbackCount) {
				session->PostToSession([self, previousRoom, rollbackResult, rollbackCount] {
					self->OnRollbackPreviousRoomCompleted(previousRoom, rollbackResult, rollbackCount);
				});
			});
		if (posted)
			return;
	}

	_RoomTransferPending = false;
	_GameRoom.reset();
	Protocol::SCJoinRoomResult response;
	response.set_result(result == UGameRoom::EEnterResult::RoomFull ? Protocol::JOIN_ROOM_FULL : Protocol::JOIN_ROOM_NONE);
	SendPacket(Protocol::SC_JOIN_ROOM_RESULT, response);
}

// 대상 방 입장 실패 뒤 이전 방 복귀 결과를 반영한다.
void UPlayer::OnRollbackPreviousRoomCompleted(const Core::SHPTR<UGameRoom>& previousRoom,
	const UGameRoom::EEnterResult result, const Core::uint32 userCount)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnRollbackPreviousRoomCompleted, GetRuntimeTraceID());
	_RoomTransferPending = false;
	if (_Disconnected) {
		if (result == UGameRoom::EEnterResult::Success && previousRoom)
			previousRoom->RequestLeave(ThisShared<UPlayer>());
		return;
	}

	Protocol::SCJoinRoomResult response;
	response.set_result(Protocol::JOIN_ROOM_FULL);
	if (result == UGameRoom::EEnterResult::Success && previousRoom) {
		_GameRoom = previousRoom;
		response.set_room_id(static_cast<Core::uint32>(previousRoom->GetRoomID()));
		response.set_user_count(userCount);
	}
	else {
		_GameRoom.reset();
	}
	SendPacket(Protocol::SC_JOIN_ROOM_RESULT, response);
}

// 예약된 전역 닉네임을 원자 플래그로 정확히 1회만 해제한다.
void UPlayer::ReleaseNicknameOnce() noexcept
{
	if (_NicknameRegistry && _NicknameReserved.exchange(false, std::memory_order_acq_rel))
		_NicknameRegistry->Release(_Info.Nickname.View());
}

SPACE_T
