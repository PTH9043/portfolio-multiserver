#include "UServerSession.h"
#include "ChatProtocolLimits.h"
#include "UPlayer.h"
#include "UCoreInstance.h"
#include "URoomManager.h"
#include "ULogger.h"
#include "Packet.h"

#include "packet_type.pb.h"
#include "login.pb.h"
#include "chat.pb.h"
#include "room.pb.h"

#include <cstring>
#include <string>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerSession.cpp"

// protobuf 생성 타입의 짧은 이름. 이 파일 안에서만 쓴다. (2026-11-19 사용자 선택)
namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UServerSession::UServerSession(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::TCPSOCKET socket)
	: USession{ std::move(coreInstance), std::move(socket) }
{
	RUNTIME_TRACE_EVENT(Create, UServerSession, UServerSession, GetRuntimeTraceID());
}

UServerSession::~UServerSession()
{
	RUNTIME_TRACE_EVENT(Destroy, UServerSession, ~UServerSession, GetRuntimeTraceID());
}

void UServerSession::ProcessingData(Core::uint8* packetData, Core::uint64 packetSize)
{
	RUNTIME_TRACE_SCOPE(UServerSession, ProcessingData, GetRuntimeTraceID());
	Core::FPacketHeader header{};
	std::memcpy(&header, packetData, Core::PACKET_HEADER_SIZE);

	// USession::CombinePacket이 머리말의 본문 크기를 PACKET_MAX_BODY_SIZE 안으로 확인한 뒤 넘긴다.
	const Core::uint8* body{ packetData + Core::PACKET_HEADER_SIZE };
	const Core::uint64 bodySize{ packetSize - Core::PACKET_HEADER_SIZE };

	switch (header.type) {
	case Protocol::CS_LOGIN:
		HandlePacket<Protocol::CSLogin>(header.type, body, bodySize);
		break;
	case Protocol::CS_CHAT:
		HandlePacket<Protocol::CSChat>(header.type, body, bodySize);
		break;
	case Protocol::CS_ROOM_USERS:
		HandlePacket<Protocol::CSRoomUsers>(header.type, body, bodySize);
		break;
	default:
		RejectPacket(header.type, "undefined packet type");
		break;
	}
}

void UServerSession::OnDisconnected()
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnDisconnected, GetRuntimeTraceID());
	_Disconnected = true;
	_LoginPending = false;
	const auto player = std::move(_Player);
	const auto gameRoom = std::move(_GameRoom);
	if (player && gameRoom)
		gameRoom->RequestLeave(player);
}

template<class TPacket>
void UServerSession::HandlePacket(Core::uint16 packetType, const Core::uint8* body, Core::uint64 bodySize)
{
	RUNTIME_TRACE_SCOPE(UServerSession, HandlePacket, GetRuntimeTraceID());
	// bodySize는 PACKET_MAX_BODY_SIZE(1020) 이하라 int로 바꿔도 값이 잘리지 않는다.
	// proto3 string이 UTF-8이 아니어도 해석에 실패한다.
	TPacket packet;
	if (!packet.ParseFromArray(body, static_cast<int>(bodySize))) {
		RejectPacket(packetType, "parse failed");
		return;
	}

	OnPacket(packet);
}

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

void UServerSession::OnPacket(const Protocol::CSLogin& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnPacket, GetRuntimeTraceID());
	Protocol::SCLoginResult response;
	if (_Player || _LoginPending) {
		response.set_result(Protocol::LOGIN_RESULT_ALREADY_LOGGED_IN);
		SendPacket(Protocol::SC_LOGIN_RESULT, response);
		return;
	}

	const Core::STRINGVIEW nickname{ packet.nickname() };
	if (!IsValidNickname(nickname)) {
		response.set_result(Protocol::LOGIN_RESULT_INVALID_NICKNAME);
		SendPacket(Protocol::SC_LOGIN_RESULT, response);
		return;
	}

	const auto coreInstance = GetCoreInstance();
	const auto roomManager = coreInstance ? coreInstance->GetRoomManager() : nullptr;
	const auto baseRoom = roomManager ? roomManager->FindRoom(DEFAULT_GAME_ROOM_ID) : nullptr;
	const auto gameRoom = std::dynamic_pointer_cast<UGameRoom>(baseRoom);
	if (!gameRoom) {
		RejectPacket(Protocol::CS_LOGIN, "default game room not found");
		return;
	}

	const auto self = ThisShared<UServerSession>();
	FPlayerInfo info{ Core::FString{ nickname } };
	const auto player = Core::MakeShared<UPlayer>(coreInstance, std::move(info), ThisWeak<Core::USession>());
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

void UServerSession::OnPacket(const Protocol::CSChat& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnPacket, GetRuntimeTraceID());
	if (!_Player || !_GameRoom) {
		RejectPacket(Protocol::CS_CHAT, "not logged in");
		return;
	}

	const Core::STRINGVIEW text{ packet.text() };
	const EChatTextError textError = ValidateChatText(text);
	if (textError != EChatTextError::None) {
		const auto coreInstance = GetCoreInstance();
		const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
		if (logger) {
			Core::FString message{ "UServerSession ignored invalid chat text (" };
			message << GetChatTextErrorReason(textError) << ", " << std::to_string(text.size()) << " bytes)";
			logger->TryLog(Core::ELogLevel::Warning, message.View());
		}
		return;
	}

	_GameRoom->RequestBroadcastChat(_Player, Core::FString{ text });
}

void UServerSession::OnPacket(const Protocol::CSRoomUsers& packet)
{
	RUNTIME_TRACE_SCOPE(UServerSession, OnPacket, GetRuntimeTraceID());
	if (!_Player || !_GameRoom) {
		RejectPacket(Protocol::CS_ROOM_USERS, "not logged in");
		return;
	}

	_GameRoom->RequestRoomUsers(_Player, packet.offset());
}

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
		_Player = player;
		_GameRoom = gameRoom;
		response.set_result(Protocol::LOGIN_RESULT_SUCCESS);
		response.set_user_count(userCount);
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
