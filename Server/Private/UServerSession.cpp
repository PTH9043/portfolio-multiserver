#include "UServerSession.h"
#include "ChatProtocolLimits.h"
#include "UDatabaseManager.h"
#include "UPlayer.h"
#include "UCoreInstance.h"
#include "URoomManager.h"
#include "ULogger.h"
#include "Packet.h"

#include "packet_type.pb.h"
#include "login.pb.h"
#include "room.pb.h"

#include <string>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerSession.cpp"

// protobuf 생성 타입의 짧은 이름. 이 파일 안에서만 쓴다. (2026-11-19 사용자 선택)
namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UServerSession::UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket,
	Core::SHPTR<UNicknameRegistry> nicknameRegistry, Core::SHPTR<UDatabaseManager> databaseManager,
	Core::FString remoteIp)
	: USession{ std::move(coreInstance), std::move(socket) },
	  _NicknameRegistry{ std::move(nicknameRegistry) },
	  _DatabaseManager{ std::move(databaseManager) },
	  _RemoteIp{ std::move(remoteIp) }
{
	RUNTIME_TRACE_EVENT(Create, UServerSession, UServerSession, GetRuntimeTraceID());
}

UServerSession::~UServerSession()
{
	RUNTIME_TRACE_EVENT(Destroy, UServerSession, ~UServerSession, GetRuntimeTraceID());
}

// 로그인 전(소유자 미부착) 상태에서 들어온 패킷만 처리하고, 미인증 도메인 패킷은 차단한다.
void UServerSession::OnUnboundPacket(const Core::FPacketView& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnUnboundPacket, GetRuntimeTraceID());
	switch (packet.GetType()) {
	case Protocol::CS_LOGIN:
		HandlePacket<Protocol::CSLogin>(packet);
		break;
	case Protocol::CS_REGISTER:
		HandlePacket<Protocol::CSRegister>(packet);
		break;
	case Protocol::CS_PING:
		HandlePacket<Protocol::CSPing>(packet);
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

// 연결 종료 시 진행 중인 로그인 대기 상태를 해제한다.
void UServerSession::OnDisconnected()
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnDisconnected, GetRuntimeTraceID());
	_Disconnected = true;
	_LoginPending = false;
	_RegisterPending = false;
}

// 로그인 전 패킷을 역직렬화하고 실패 시 연결을 닫는다.
template<class TPacket>
void UServerSession::HandlePacket(const Core::FPacketView& packetView)
{
	RUNTIME_TRACE_SCOPE(UServerSession, HandlePacket, GetRuntimeTraceID());
	// packetView.GetSize()는 PACKET_MAX_BODY_SIZE(1020) 이하라 int로 바꿔도 값이 잘리지 않는다.
	// proto3 string이 UTF-8이 아니어도 해석에 실패한다.
	TPacket packet;
	if (!packet.ParseFromArray(packetView.GetData(), static_cast<int>(packetView.GetSize()))) {
		RejectPacket(packetView.GetType(), "parse failed");
		return;
	}

	OnPacket(packet);
}

// 로그인 응답 등 서버 패킷을 직렬화해 송신한다.
template<class TPacket>
void UServerSession::SendPacket(Core::uint16 packetType, const TPacket& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, SendPacket, GetRuntimeTraceID());
	const Core::uint64 bodySize{ static_cast<Core::uint64>(packet.ByteSizeLong()) };
	if (bodySize > Core::PACKET_MAX_BODY_SIZE) {
		const auto coreInstance = GetCoreInstance();
		const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
		if (logger) {
			Core::FString message{ "UServerSession skipped oversized packet type " };
			message << std::to_string(packetType) << " (" << std::to_string(bodySize) << " bytes)";
			logger->TryLog(Core::ELogLevel::Warning, message.View());
		}
		return;
	}

	Core::ARRAY<Core::uint8, Core::PACKET_MAX_BODY_SIZE> buffer{};
	if (bodySize != 0 && !packet.SerializeToArray(buffer.data(), static_cast<int>(bodySize))) {
		const auto coreInstance = GetCoreInstance();
		const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
		if (logger) {
			Core::FString message{ "UServerSession failed to serialize packet type " };
			message << std::to_string(packetType);
			logger->TryLog(Core::ELogLevel::Warning, message.View());
		}
		return;
	}

	Send(bodySize == 0 ? nullptr : buffer.data(), bodySize, packetType);
}

// 회원가입 요청을 검증한 뒤 DB 전용 스레드에 비동기 계정 생성을 위임한다.
void UServerSession::OnPacket(const Protocol::CSRegister& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnPacket, GetRuntimeTraceID());
	Protocol::SCRegisterResult response;
	if (!_DatabaseManager || !_DatabaseManager->IsEnabled()) {
		response.set_result(Protocol::REGISTER_RESULT_DB_ERROR);
		SendPacket(Protocol::SC_REGISTER_RESULT, response);
		return;
	}
	if (HasOwner() || _LoginPending || _RegisterPending) {
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

	_RegisterPending = true;
	const Core::WKPTR<UServerSession> weakSelf = ThisWeak<UServerSession>();
	const bool queued = _DatabaseManager->EnqueueRegister(
		Core::FString{ username }, Core::FString{ password }, Core::FString{ nickname },
		[weakSelf](const EDbRegisterStatus status, Core::FString) {
			if (const auto self = weakSelf.lock()) {
				self->PostToSession([self, status]() {
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

void UServerSession::OnDbRegisterCompleted(const EDbRegisterStatus status)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnDbRegisterCompleted, GetRuntimeTraceID());
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

// 로그인 요청을 처리한다(--db=mysql: DB 전용 스레드 인증, --db=off: 인메모리 닉네임 인증).
void UServerSession::OnPacket(const Protocol::CSLogin& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnPacket, GetRuntimeTraceID());
	Protocol::SCLoginResult response;
	if (HasOwner() || _LoginPending) {
		response.set_result(Protocol::LOGIN_RESULT_ALREADY_LOGGED_IN);
		SendPacket(Protocol::SC_LOGIN_RESULT, response);
		return;
	}

	// MySQL 활성 모드: username + password를 DB 전용 스레드로 넘겨 PBKDF2 검증 후 입장한다.
	if (_DatabaseManager && _DatabaseManager->IsEnabled()) {
		const Core::STRINGVIEW username{ packet.username() };
		const Core::STRINGVIEW password{ packet.password() };
		if (!IsValidUsername(username) || !IsValidPassword(password)) {
			response.set_result(Protocol::LOGIN_RESULT_INVALID_CREDENTIALS);
			SendPacket(Protocol::SC_LOGIN_RESULT, response);
			return;
		}

		_LoginPending = true;
		const Core::WKPTR<UServerSession> weakSelf = ThisWeak<UServerSession>();
		const auto dbManager = _DatabaseManager;
		const bool queued = dbManager->EnqueueLogin(
			Core::FString{ username }, Core::FString{ password }, _RemoteIp,
			[weakSelf, dbManager](FDbLoginOutcome outcome) {
				if (const auto self = weakSelf.lock()) {
					self->PostToSession([self, outcome = std::move(outcome)]() mutable {
						self->OnDbLoginCompleted(outcome);
					});
				}
				else if (outcome.Status == EDbLoginStatus::Success && outcome.ConnectionLogID != 0 && dbManager) {
					// DB 응답 도착 전 세션 객체가 이미 소멸했다면 생성된 접속 이력을 즉시 종료 처리한다.
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

void UServerSession::OnDbLoginCompleted(const FDbLoginOutcome& outcome)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnDbLoginCompleted, GetRuntimeTraceID());
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

void UServerSession::BeginRoomEnter(Core::FString nickname, const Core::uint64 accountId, const Core::uint64 connectionLogId)
{
	Protocol::SCLoginResult response;
	const auto coreInstance = GetCoreInstance();
	const auto roomManager = coreInstance ? coreInstance->GetRoomManager() : nullptr;
	const auto gameRoom = roomManager ? roomManager->FindRoomAs<UGameRoom>(DEFAULT_GAME_ROOM_ID) : nullptr;
	if (!gameRoom) {
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

	const auto self = ThisShared<UServerSession>();
	FPlayerInfo info{ std::move(nickname), accountId, connectionLogId };
	const auto player = Core::MakeShared<UPlayer>(coreInstance, std::move(info), _NicknameRegistry, _DatabaseManager);
	if (!player->AddComponent(self)) {
		_LoginPending = false;
		RejectPacket(Protocol::CS_LOGIN, "failed to attach session component");
		return;
	}
	_LoginPending = true;

	const bool posted = gameRoom->RequestEnterPlayer(player,
		[self, gameRoom, player](UGameRoom::EEnterResult result, Core::uint32 userCount) {
			self->PostToSession([self, gameRoom, player, result, userCount] {
				self->OnEnterRoomCompleted(gameRoom, player, result, userCount);
			});
		});
	if (!posted) {
		_LoginPending = false;
		RejectPacket(Protocol::CS_LOGIN, "failed to post room enter");
	}
}

// 로그인 전 클라이언트 하트비트 Ping에도 즉시 Pong으로 응답한다.
void UServerSession::OnPacket(const Protocol::CSPing& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnPacket, GetRuntimeTraceID());
	Protocol::SCPong response;
	response.set_client_time_ms(packet.client_time_ms());
	SendPacket(Protocol::SC_PONG, response);
}

// 비동기 방 입장 결과를 클라이언트에 응답하고, 도중 연결이 끊겼으면 즉시 방 퇴장을 요청한다.
void UServerSession::OnEnterRoomCompleted(const Core::SHPTR<UGameRoom>& gameRoom, const Core::SHPTR<UPlayer>& player,
	UGameRoom::EEnterResult result, Core::uint32 userCount)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnEnterRoomCompleted, GetRuntimeTraceID());
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
		player->SetGameRoom(gameRoom);
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

// 프로토콜 위반 사유를 경고 기록하고 세션을 닫는다.
void UServerSession::RejectPacket(Core::uint16 packetType, Core::STRINGVIEW reason)
{
	RUNTIME_TRACE_SCOPE(UServerSession, RejectPacket, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (logger) {
		Core::FString message{ "UServerSession rejected packet type " };
		message << std::to_string(packetType) << " (" << reason << "); closing the connection";
		logger->TryLog(Core::ELogLevel::Warning, message.View());
	}
	Close();
}

SPACE_T
