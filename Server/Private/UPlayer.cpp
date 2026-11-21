#include "UPlayer.h"
#include "ChatProtocolLimits.h"
#include "UGameRoom.h"
#include "USession.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include "Packet.h"

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

UPlayer::UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance, FPlayerInfo info)
	: UCharacter{ std::move(coreInstance) },
	  _Info{ std::move(info) }
{
	RUNTIME_TRACE_EVENT(Create, UPlayer, UPlayer, GetRuntimeTraceID());
}

UPlayer::~UPlayer()
{
	RUNTIME_TRACE_EVENT(Destroy, UPlayer, ~UPlayer, GetRuntimeTraceID());
}

// 입장 확정 시 소속 게임 방을 약참조로 연결한다.
void UPlayer::SetGameRoom(const Core::SHPTR<UGameRoom>& gameRoom) noexcept
{
	_GameRoom = gameRoom;
}

// 로그인 이후 세션에서 올라온 패킷을 타입별로 분기 처리한다.
void UPlayer::OnReceivePacket(const Core::FPacketView& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnReceivePacket, GetRuntimeTraceID());
	switch (packet.GetType()) {
	case Protocol::CS_LOGIN:
		HandlePacket<Protocol::CSLogin>(packet);
		break;
	case Protocol::CS_CHAT:
		HandlePacket<Protocol::CSChat>(packet);
		break;
	case Protocol::CS_ROOM_USERS:
		HandlePacket<Protocol::CSRoomUsers>(packet);
		break;
	default:
		RejectPacket(packet.GetType(), "undefined packet type");
		break;
	}
}

// 세션 종료 시 소속 방에서 플레이어 퇴장을 요청한다.
void UPlayer::OnSessionDisconnected()
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnSessionDisconnected, GetRuntimeTraceID());
	const auto gameRoom = _GameRoom.lock();
	_GameRoom.reset();
	if (gameRoom)
		gameRoom->RequestLeave(ThisShared<UPlayer>());
}

// 패킷 뷰를 Protobuf 메시지로 역직렬화하고 실패 시 연결을 닫는다.
template<class TPacket>
void UPlayer::HandlePacket(const Core::FPacketView& packetView)
{
	RUNTIME_TRACE_SCOPE(UPlayer, HandlePacket, GetRuntimeTraceID());
	TPacket packet;
	if (!packet.ParseFromArray(packetView.GetData(), static_cast<int>(packetView.GetSize()))) {
		RejectPacket(packetView.GetType(), "parse failed");
		return;
	}

	OnPacket(packet);
}

// 응답 패킷을 직렬화해 부착된 세션으로 송신한다.
template<class TPacket>
void UPlayer::SendPacket(const Core::uint16 packetType, const TPacket& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, SendPacket, GetRuntimeTraceID());
	const auto session = GetSession();
	if (!session)
		return;

	const Core::uint64 bodySize{ static_cast<Core::uint64>(packet.ByteSizeLong()) };
	if (bodySize > Core::PACKET_MAX_BODY_SIZE) {
		const auto coreInstance = GetCoreInstance();
		const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
		if (logger) {
			Core::FString message{ "UPlayer skipped oversized packet type " };
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
			Core::FString message{ "UPlayer failed to serialize packet type " };
			message << std::to_string(packetType);
			logger->TryLog(Core::ELogLevel::Warning, message.View());
		}
		return;
	}

	session->Send(bodySize == 0 ? nullptr : buffer.data(), bodySize, packetType);
}

// 이미 로그인된 플레이어의 중복 로그인 요청을 거절한다.
void UPlayer::OnPacket(const Protocol::CSLogin& packet)
{
	(void)packet;
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	Protocol::SCLoginResult response;
	response.set_result(Protocol::LOGIN_RESULT_ALREADY_LOGGED_IN);
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

// 프로토콜 위반 패킷을 경고 기록하고 세션을 닫는다.
void UPlayer::RejectPacket(const Core::uint16 packetType, const Core::STRINGVIEW reason)
{
	RUNTIME_TRACE_SCOPE(UPlayer, RejectPacket, GetRuntimeTraceID());
	const auto coreInstance = GetCoreInstance();
	const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
	if (logger) {
		Core::FString message{ "UPlayer rejected packet type " };
		message << std::to_string(packetType) << " (" << reason << "); closing the connection";
		logger->TryLog(Core::ELogLevel::Warning, message.View());
	}
	if (const auto session = GetSession())
		session->Close();
}

SPACE_T
