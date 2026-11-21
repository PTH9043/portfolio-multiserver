#include "UPlayer.h"
#include "ChatProtocolLimits.h"
#include "UDatabaseManager.h"
#include "UGameRoom.h"
#include "USession.h"
#include "UCoreInstance.h"
#include "URoomManager.h"
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

UPlayer::UPlayer(Core::SHPTR<Core::UCoreInstance> coreInstance, FPlayerInfo info,
	Core::SHPTR<UNicknameRegistry> nicknameRegistry, Core::SHPTR<UDatabaseManager> databaseManager)
	: UCharacter{ std::move(coreInstance) },
	  _Info{ std::move(info) },
	  _NicknameRegistry{ std::move(nicknameRegistry) },
	  _DatabaseManager{ std::move(databaseManager) }
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

// 입장 확정 시 소속 게임 방을 약참조로 연결한다.
void UPlayer::SetGameRoom(const Core::SHPTR<UGameRoom>& gameRoom) noexcept
{
	_GameRoom = gameRoom;
	if (gameRoom)
		_LastRoomID = static_cast<Core::uint32>(gameRoom->GetRoomID());
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
	case Protocol::CS_ROOM_LIST:
		HandlePacket<Protocol::CSRoomList>(packet);
		break;
	case Protocol::CS_JOIN_ROOM:
		HandlePacket<Protocol::CSJoinRoom>(packet);
		break;
	case Protocol::CS_PING:
		HandlePacket<Protocol::CSPing>(packet);
		break;
	default:
		RejectPacket(packet.GetType(), "undefined packet type");
		break;
	}
}

// 세션 종료 시 전역 닉네임을 즉시 해제하고 소속 방 퇴장 및 비동기 DB 로그아웃 이력을 요청한다.
void UPlayer::OnSessionDisconnected()
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnSessionDisconnected, GetRuntimeTraceID());
	_Disconnected = true;
	_RoomTransferPending = false;
	ReleaseNicknameOnce();
	const auto gameRoom = _GameRoom.lock();
	if (gameRoom)
		_LastRoomID = static_cast<Core::uint32>(gameRoom->GetRoomID());
	_GameRoom.reset();
	if (gameRoom)
		gameRoom->RequestLeave(ThisShared<UPlayer>());
	if (_DatabaseManager && _Info.ConnectionLogID != 0
		&& !_LogoutRecorded.exchange(true, std::memory_order_acq_rel)) {
		_DatabaseManager->EnqueueLogout(_Info.ConnectionLogID, _LastRoomID, _DisconnectReason);
	}
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

// 클라이언트 하트비트 Ping에 타임스탬프를 그대로 돌려보낸다.
void UPlayer::OnPacket(const Protocol::CSPing& packet)
{
	RUNTIME_TRACE_SCOPE(UPlayer, OnPacket, GetRuntimeTraceID());
	Protocol::SCPong response;
	response.set_client_time_ms(packet.client_time_ms());
	SendPacket(Protocol::SC_PONG, response);
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
	if (_NicknameRegistry && !_NicknameReleased.exchange(true, std::memory_order_acq_rel))
		_NicknameRegistry->Release(_Info.Nickname.View());
}

// 프로토콜 위반 패킷을 경고 기록하고 세션을 닫는다.
void UPlayer::RejectPacket(const Core::uint16 packetType, const Core::STRINGVIEW reason)
{
	RUNTIME_TRACE_SCOPE(UPlayer, RejectPacket, GetRuntimeTraceID());
	_DisconnectReason = Core::FString{ "PROTOCOL_ERROR" };
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
