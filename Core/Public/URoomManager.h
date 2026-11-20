#pragma once

#include "UObject.h"

SPACE_H(Core)

class URoom;

// 서버가 운영하는 방(URoom) 인스턴스를 RoomID 기준으로 등록·조회한다.
// 런타임 방 삭제를 지원하지 않는 Append-Only 정책을 전제로, 단일 CONUNORMAP으로
// AddRoom·FindRoom·GetRoomCount를 mutex 없이 동시 실행한다.
// ClearRooms와 소멸자는 모든 워커 스레드가 종료된 뒤 제어 스레드 단독 시점에만 호출한다.
class CORE_DLL URoomManager final : public UObject {
	using ROOM_CONTAINER = CONUNORMAP<uint64, SHPTR<URoom>>;

public:
	explicit URoomManager(SHPTR<UCoreInstance> coreInstance);
	virtual ~URoomManager() override;
	NO_COPY(URoomManager)

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. room이 비어 있거나 RoomID가 0이거나 같은 ID가 이미 있으면 false다.
	// ClearRooms·소멸자와 동시에 호출하면 안 된다.
	bool AddRoom(const SHPTR<URoom>& room);

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 해당 ID의 방이 없으면 빈 포인터를 돌려준다.
	// ClearRooms·소멸자와 동시에 호출하면 안 된다.
	SHPTR<URoom> FindRoom(uint64 roomID) const;

	// 모든 워커가 종료(JoinAll)된 뒤 제어 스레드 단독 시점에만 호출한다.
	// CONUNORMAP::clear는 비동시성이므로 FindRoom·AddRoom과 동시에 호출하면 안 된다.
	void ClearRooms();

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 현재 등록된 방의 개수를 돌려준다.
	uint64 GetRoomCount() const noexcept;

private:
	ROOM_CONTAINER _RoomContainer;
};

SPACE_T
