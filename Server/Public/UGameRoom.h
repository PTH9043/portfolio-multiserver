#pragma once

#include "CoreDefine.h"
#include "URoom.h"

SPACE_H(Server)

class UPlayer;

// 채팅·인원 관리 도메인 규칙을 담당하는 구체 방 클래스다.
// 기반 클래스 Core::URoom의 단일 _ActorContainer와 _Strand 직렬화를 그대로 재사용하며,
// 닉네임 중복 방지 인덱스(_NicknameIndex)를 포함한 모든 도메인 상태는 이 방의 strand에서만 접근한다(내부 mutex 0개).
class UGameRoom final : public Core::URoom {
public:
	enum class EEnterResult : Core::uint8 {
		Success,
		DuplicateNickname,
		RoomFull,
		AlreadyEntered,
		InvalidActor
	};

	explicit UGameRoom(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context, Core::uint64 roomID, Core::uint32 maxActorCount);
	virtual ~UGameRoom() override;
	NO_COPY(UGameRoom)

	// 어느 스레드에서나 호출할 수 있다. 플레이어 입장 판정(중복 닉네임·정원·중복 입장)을 이 방의 strand에서 실행하고,
	// 결과를 onComplete(result, userCount)로 이 방의 strand에서 전달한다.
	// 입장 성공 시 기존 참가자들에게 SC_ROOM_USER_COUNT를 브로드캐스트한다.
	// Room이 shared_ptr 소유가 아니거나 player가 비어 있으면 false다.
	bool RequestEnterPlayer(const Core::SHPTR<UPlayer>& player, Core::FUNC<void(EEnterResult, Core::uint32)> onComplete);

	// 어느 스레드에서나 호출할 수 있다. 보낸 사람이 현재 이 방에 속해 있으면 SC_CHAT을 방 전체에 브로드캐스트한다.
	// Room이 shared_ptr 소유가 아니거나 sender가 비어 있으면 false다.
	bool RequestBroadcastChat(const Core::SHPTR<UPlayer>& sender, Core::FString text);

	// 어느 스레드에서나 호출할 수 있다. 요청자가 현재 이 방에 속해 있으면 offset부터 최대 ROOM_USERS_PAGE_SIZE명의
	// 닉네임을 SC_ROOM_USERS로 요청자 세션에 보낸다.
	// Room이 shared_ptr 소유가 아니거나 requester가 비어 있으면 false다.
	bool RequestRoomUsers(const Core::SHPTR<UPlayer>& requester, Core::uint32 offset);

protected:
	// 이 Room의 strand에서 호출된다. UPlayer인 경우 닉네임 중복 여부를 검사한다.
	bool CanEnter(const Core::SHPTR<Core::UActor>& actor) const override;

	// 이 Room의 strand에서 호출된다. UPlayer의 닉네임을 _NicknameIndex에 등록하고,
	// 기본 URoom::RequestEnter 경로에서 호출된 경우 인원 수 변경을 방 전체에 브로드캐스트한다.
	void OnActorEntered(const Core::SHPTR<Core::UActor>& actor) override;

	// 이 Room의 strand에서 호출된다. UPlayer의 닉네임을 _NicknameIndex에서 제거하고
	// 남은 참가자들에게 변경된 SC_ROOM_USER_COUNT를 브로드캐스트한다.
	void OnActorLeft(const Core::SHPTR<Core::UActor>& actor) override;

private:
	// 이 Room의 strand에서 호출한다. 인원 수 변경 브로드캐스트를 ROOM_USER_COUNT_COALESCE_INTERVAL 간격으로 병합 예약한다.
	void ScheduleUserCountBroadcastOnStrand();

	// 이 Room의 strand에서 호출한다. 예약된 인원 수 변경을 현재 방 전체에 공유 프레임 한 번으로 브로드캐스트한다.
	void FlushUserCountOnStrand();

	// 직렬화 크기 검증(PACKET_MAX_BODY_SIZE 이하) 후 buffer에 직렬화한다. 실패 시 Warning을 남기고 false다.
	template<class TPacket>
	bool SerializePacket(Core::uint16 packetType, const TPacket& packet,
		Core::ARRAY<Core::uint8, Core::PACKET_MAX_BODY_SIZE>& outBuffer, Core::uint64& outSize) const;

	Core::UNORSET<Core::FString> _NicknameIndex; // 이 Room의 strand에서만 접근한다.
	Core::BOOST_STEADY_TIMER _CountBroadcastTimer;
	bool _CountBroadcastPending{ false };        // 이 Room의 strand에서만 접근한다.
	bool _DeferEnteredCountBroadcast{ false };   // 이 Room의 strand에서만 접근한다.
};

SPACE_T
