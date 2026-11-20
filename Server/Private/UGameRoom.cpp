#include "UGameRoom.h"
#include "ChatProtocolLimits.h"
#include "UPlayer.h"
#include "USession.h"
#include "UCoreInstance.h"
#include "ULogger.h"

#include "packet_type.pb.h"
#include "chat.pb.h"
#include "room.pb.h"

#include <string>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UGameRoom.cpp"

namespace Protocol = multiplayer::protocol;

SPACE_H(Server)

UGameRoom::UGameRoom(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context, Core::uint64 roomID, Core::uint32 maxActorCount)
	: URoom{ std::move(coreInstance), context, roomID, maxActorCount },
	  _CountBroadcastTimer{ context }
{
	RUNTIME_TRACE_EVENT(Create, UGameRoom, UGameRoom, GetRuntimeTraceID());
}

UGameRoom::~UGameRoom()
{
	_CountBroadcastTimer.cancel();
	RUNTIME_TRACE_EVENT(Destroy, UGameRoom, ~UGameRoom, GetRuntimeTraceID());
}

bool UGameRoom::RequestEnterPlayer(const Core::SHPTR<UPlayer>& player, Core::FUNC<void(EEnterResult, Core::uint32)> onComplete)
{
	RUNTIME_TRACE_SCOPE(UGameRoom, RequestEnterPlayer, GetRuntimeTraceID());
	if (!player)
		return false;

	const auto submission = RUNTIME_TRACE_EVENT(Submit, UGameRoom, RequestEnterPlayer, GetRuntimeTraceID());
	return PostToRoom([this, player, onComplete = std::move(onComplete), submission]() mutable {
		RUNTIME_TRACE_CALLBACK(UGameRoom, RequestEnterPlayer, GetRuntimeTraceID(), submission);
		const Core::uint32 currentCount{ static_cast<Core::uint32>(GetActorCount()) };
		if (!player->GetSession()) {
			if (onComplete)
				onComplete(EEnterResult::InvalidActor, currentCount);
			return;
		}
		if (HasActor(player.get())) {
			if (onComplete)
				onComplete(EEnterResult::AlreadyEntered, currentCount);
			return;
		}
		if (_NicknameIndex.contains(player->GetInfo().Nickname)) {
			if (onComplete)
				onComplete(EEnterResult::DuplicateNickname, currentCount);
			return;
		}
		if (GetActorCount() >= GetMaxActorCount()) {
			if (onComplete)
				onComplete(EEnterResult::RoomFull, currentCount);
			return;
		}

		// 입장하는 본인에게는 SC_LOGIN_RESULT가 먼저 도착하도록 onComplete 후처리에서 인원 수를 보내고,
		// 방 전체 인원 수 브로드캐스트는 짧은 간격으로 병합해 동시 입장 폭주 시 O(N^2) 송신을 방지한다.
		_DeferEnteredCountBroadcast = true;
		const bool entered = TryEnterOnStrand(player);
		_DeferEnteredCountBroadcast = false;
		if (!entered) {
			if (onComplete)
				onComplete(EEnterResult::RoomFull, static_cast<Core::uint32>(GetActorCount()));
			return;
		}

		const Core::uint32 updatedCount{ static_cast<Core::uint32>(GetActorCount()) };
		ScheduleUserCountBroadcastOnStrand();
		if (onComplete)
			onComplete(EEnterResult::Success, updatedCount);
	});
}

bool UGameRoom::RequestBroadcastChat(const Core::SHPTR<UPlayer>& sender, Core::FString text)
{
	RUNTIME_TRACE_SCOPE(UGameRoom, RequestBroadcastChat, GetRuntimeTraceID());
	if (!sender)
		return false;

	const auto submission = RUNTIME_TRACE_EVENT(Submit, UGameRoom, RequestBroadcastChat, GetRuntimeTraceID());
	return PostToRoom([this, sender, text = std::move(text), submission]() mutable {
		RUNTIME_TRACE_CALLBACK(UGameRoom, RequestBroadcastChat, GetRuntimeTraceID(), submission);
		if (!HasActor(sender.get()))
			return;

		Protocol::SCChat packet;
		const Core::STRINGVIEW nickname{ sender->GetInfo().Nickname.View() };
		packet.set_nickname(nickname.data(), nickname.size());
		packet.set_text(text.data(), text.size());

		Core::ARRAY<Core::uint8, Core::PACKET_MAX_BODY_SIZE> buffer{};
		Core::uint64 bodySize{ 0 };
		if (!SerializePacket(Protocol::SC_CHAT, packet, buffer, bodySize))
			return;

		BroadcastOnStrand(bodySize == 0 ? nullptr : buffer.data(), bodySize, Protocol::SC_CHAT);
	});
}

bool UGameRoom::RequestRoomUsers(const Core::SHPTR<UPlayer>& requester, Core::uint32 offset)
{
	RUNTIME_TRACE_SCOPE(UGameRoom, RequestRoomUsers, GetRuntimeTraceID());
	if (!requester)
		return false;

	const auto submission = RUNTIME_TRACE_EVENT(Submit, UGameRoom, RequestRoomUsers, GetRuntimeTraceID());
	return PostToRoom([this, requester, offset, submission] {
		RUNTIME_TRACE_CALLBACK(UGameRoom, RequestRoomUsers, GetRuntimeTraceID(), submission);
		if (!HasActor(requester.get()))
			return;

		const auto session = requester->GetSession();
		if (!session)
			return;

		const ACTOR_CONTAINER& actors = GetActorContainer();
		const Core::uint64 totalCount{ static_cast<Core::uint64>(actors.size()) };

		Protocol::SCRoomUsers packet;
		packet.set_offset(offset);
		packet.set_total(static_cast<Core::uint32>(totalCount));

		Core::uint32 collected{ 0 };
		for (Core::uint64 index = offset; index < totalCount && collected < ROOM_USERS_PAGE_SIZE; ++index) {
			const auto* player = dynamic_cast<const UPlayer*>(actors[index].get());
			if (!player)
				continue;
			const Core::STRINGVIEW nickname{ player->GetInfo().Nickname.View() };
			packet.add_nicknames(nickname.data(), nickname.size());
			++collected;
		}

		Core::ARRAY<Core::uint8, Core::PACKET_MAX_BODY_SIZE> buffer{};
		Core::uint64 bodySize{ 0 };
		if (!SerializePacket(Protocol::SC_ROOM_USERS, packet, buffer, bodySize))
			return;

		session->Send(bodySize == 0 ? nullptr : buffer.data(), bodySize, Protocol::SC_ROOM_USERS);
	});
}

bool UGameRoom::CanEnter(const Core::SHPTR<Core::UActor>& actor) const
{
	if (!actor)
		return false;

	const auto* player = dynamic_cast<const UPlayer*>(actor.get());
	if (player && _NicknameIndex.contains(player->GetInfo().Nickname))
		return false;

	return true;
}

void UGameRoom::OnActorEntered(const Core::SHPTR<Core::UActor>& actor)
{
	RUNTIME_TRACE_SCOPE(UGameRoom, OnActorEntered, GetRuntimeTraceID());
	const auto* player = dynamic_cast<const UPlayer*>(actor.get());
	if (player)
		_NicknameIndex.insert(player->GetInfo().Nickname);

	if (!_DeferEnteredCountBroadcast)
		ScheduleUserCountBroadcastOnStrand();
}

void UGameRoom::OnActorLeft(const Core::SHPTR<Core::UActor>& actor)
{
	RUNTIME_TRACE_SCOPE(UGameRoom, OnActorLeft, GetRuntimeTraceID());
	const auto* player = dynamic_cast<const UPlayer*>(actor.get());
	if (player)
		_NicknameIndex.erase(player->GetInfo().Nickname);

	ScheduleUserCountBroadcastOnStrand();
}

void UGameRoom::ScheduleUserCountBroadcastOnStrand()
{
	RUNTIME_TRACE_SCOPE(UGameRoom, ScheduleUserCountBroadcastOnStrand, GetRuntimeTraceID());
	if (_CountBroadcastPending)
		return;

	_CountBroadcastPending = true;
	_CountBroadcastTimer.expires_after(ROOM_USER_COUNT_COALESCE_INTERVAL);
	const auto weakRoom = ThisWeak<UGameRoom>();
	_CountBroadcastTimer.async_wait([weakRoom](const Core::BOOST_ERROR_CODE& error) {
		if (error)
			return;
		if (const auto room = weakRoom.lock()) {
			room->PostToRoom([room] {
				room->FlushUserCountOnStrand();
			});
		}
	});
}

void UGameRoom::FlushUserCountOnStrand()
{
	RUNTIME_TRACE_SCOPE(UGameRoom, FlushUserCountOnStrand, GetRuntimeTraceID());
	_CountBroadcastPending = false;
	const Core::uint64 actorCount{ GetActorCount() };
	if (actorCount == 0)
		return;

	Protocol::SCRoomUserCount packet;
	packet.set_count(static_cast<Core::uint32>(actorCount));

	Core::ARRAY<Core::uint8, Core::PACKET_MAX_BODY_SIZE> buffer{};
	Core::uint64 bodySize{ 0 };
	if (!SerializePacket(Protocol::SC_ROOM_USER_COUNT, packet, buffer, bodySize))
		return;

	const auto framedBuffer = Core::USession::MakePacketBuffer(
		bodySize == 0 ? nullptr : buffer.data(), bodySize, Protocol::SC_ROOM_USER_COUNT);
	if (framedBuffer)
		BroadcastSharedOnStrand(framedBuffer);
}

template<class TPacket>
bool UGameRoom::SerializePacket(Core::uint16 packetType, const TPacket& packet,
	Core::ARRAY<Core::uint8, Core::PACKET_MAX_BODY_SIZE>& outBuffer, Core::uint64& outSize) const
{
	const Core::uint64 bodySize{ static_cast<Core::uint64>(packet.ByteSizeLong()) };
	if (bodySize > Core::PACKET_MAX_BODY_SIZE) {
		const auto coreInstance = GetCoreInstance();
		const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
		if (logger) {
			Core::FString message{ "UGameRoom skipped oversized packet type " };
			message << std::to_string(packetType) << " (" << std::to_string(bodySize) << " bytes)";
			logger->TryLog(Core::ELogLevel::Warning, message.View());
		}
		return false;
	}

	if (bodySize != 0 && !packet.SerializeToArray(outBuffer.data(), static_cast<int>(bodySize))) {
		const auto coreInstance = GetCoreInstance();
		const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
		if (logger) {
			Core::FString message{ "UGameRoom failed to serialize packet type " };
			message << std::to_string(packetType);
			logger->TryLog(Core::ELogLevel::Warning, message.View());
		}
		return false;
	}

	outSize = bodySize;
	return true;
}

SPACE_T
