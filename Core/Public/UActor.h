#pragma once

#include "UObject.h"

SPACE_H(Core)

class USession;

// 세상에 존재하는 모든 사물의 기반 클래스다(화면에 그려지지 않는 사물 포함).
// 연결(USession)과 상속으로 묶지 않고 분리한다. 통신이 필요한 파생 액터만 GetSession을 재정의한다.
class CORE_DLL UActor abstract : public UObject {
protected:
	explicit UActor(SHPTR<UCoreInstance> coreInstance);
	virtual ~UActor() override;

public:
	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 0이면 현재 어떤 방에도 속하지 않은 상태다.
	uint64 GetRoomID() const noexcept { return _RoomID.load(std::memory_order_acquire); }

	// 어느 스레드에서나 잠금 없이 호출할 수 있다. 방 입·퇴장 시 URoom이 소속 방 ID를 갱신한다.
	void SetRoomID(uint64 roomID) noexcept { _RoomID.store(roomID, std::memory_order_release); }

	// 현재 소속 방 ID가 expectedRoomID와 일치할 때만 0으로 초기화한다.
	// 방 이동 직후 이전 방의 늦은 퇴장 처리가 새 방 ID를 덮어쓰는 경합을 막는다.
	bool ClearRoomID(uint64 expectedRoomID) noexcept
	{
		if (expectedRoomID == 0)
			return false;
		return _RoomID.compare_exchange_strong(expectedRoomID, 0, std::memory_order_acq_rel, std::memory_order_acquire);
	}

	// 통신 세션이 연결된 액터라면 유효한 세션을 돌려준다. 비통신 액터의 기본값은 빈 포인터다.
	virtual SHPTR<USession> GetSession() const noexcept;

private:
	NO_COPY(UActor)

	ATOMIC_UINT64 _RoomID{ 0 };
};

SPACE_T
