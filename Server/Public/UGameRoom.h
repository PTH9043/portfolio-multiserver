#pragma once

#include "CoreDefine.h"
#include "URoom.h"

SPACE_H(Server)

class UPlayer;

// 서버 전역 닉네임 유일성을 관리하는 경량 레지스트리다.
// 로그인 시 1회 예약하고 퇴장·실패 시 1회 해제해 다중 방 이동 중 닉네임 충돌을 원천 제거한다.
class UNicknameRegistry final {
public:
	UNicknameRegistry() = default;
	~UNicknameRegistry() = default;
	NO_COPY(UNicknameRegistry)

	// 어느 스레드에서나 호출할 수 있다. 아직 사용 중이지 않으면 예약하고 true, 중복이면 false다.
	bool TryReserve(Core::STRINGVIEW nickname);

	// 어느 스레드에서나 호출할 수 있다. 예약된 닉네임을 해제한다.
	void Release(Core::STRINGVIEW nickname);

private:
	Core::SHARED_MUTEX _Lock;
	Core::UNORSET<Core::FString> _NicknameContainer;
};

// 채팅·인원 관리 도메인 규칙을 담당하는 구체 방 클래스다.
// 기반 클래스 Core::URoom의 단일 _ActorContainer와 _Strand 직렬화를 그대로 재사용하며,
// 모든 도메인 상태는 이 방의 strand에서만 접근한다(방 내부 mutex 0개).
class UGameRoom final : public Core::URoom {
public:
	STATIC_CONSTEXPR Core::uint32 RoomTypeID{ static_cast<Core::uint32>(Core::ERoomType::UserStart) };

	enum class EEnterResult : Core::uint8 {
		Success,
		RoomFull,
		AlreadyEntered,
		InvalidActor
	};

	explicit UGameRoom(Core::SHPTR<Core::UCoreInstance> coreInstance, Core::IOCONTEXT& context,
		Core::uint64 roomID, Core::uint32 maxActorCount, Core::FString roomName = Core::FString{ "로비" });
	virtual ~UGameRoom() override;
	NO_COPY(UGameRoom)

	// RTTI 없이 UGameRoom을 판별하기 위한 고정 방 유형 ID를 돌려준다.
	Core::uint32 GetRoomTypeID() const noexcept override { return RoomTypeID; }

	// 생성 시 확정된 불변 방 이름 참조다. 이 UGameRoom이 살아 있는 동안 어느 스레드에서나 읽을 수 있다.
	const Core::FString& GetRoomName() const noexcept { return _RoomName; }

	// 어느 스레드에서나 호출할 수 있다. 플레이어 입장 판정(정원·중복 입장·세션 유효성)을 이 방의 strand에서 실행하고,
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
	// 이 Room의 strand에서 호출된다. 기본 URoom::RequestEnter 경로에서 호출된 경우 인원 수 변경을 브로드캐스트한다.
	void OnActorEntered(const Core::SHPTR<Core::UActor>& actor) override;

	// 이 Room의 strand에서 호출된다. 남은 참가자들에게 변경된 SC_ROOM_USER_COUNT를 브로드캐스트한다.
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

	Core::FString _RoomName;
	Core::BOOST_STEADY_TIMER _CountBroadcastTimer;
	bool _CountBroadcastPending{ false };        // 이 Room의 strand에서만 접근한다.
	bool _DeferEnteredCountBroadcast{ false };   // 이 Room의 strand에서만 접근한다.
};

SPACE_T
