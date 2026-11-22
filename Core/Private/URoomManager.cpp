#include "CoreDefine.h"
#include "URoomManager.h"
#include "URoom.h"

#include <algorithm>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/URoomManager.cpp"

SPACE_H(Core)

URoomManager::URoomManager(SHPTR<UCoreInstance> coreInstance)
	: UObject{ std::move(coreInstance) }
{
	RUNTIME_TRACE_EVENT(Create, URoomManager, URoomManager, GetRuntimeTraceID());
}

URoomManager::~URoomManager()
{
	RUNTIME_TRACE_EVENT(Destroy, URoomManager, ~URoomManager, GetRuntimeTraceID());
}

// 유효한 방 ID(0 제외)를 가진 방을 Append-Only 맵에 등록한다.
bool URoomManager::AddRoom(const SHPTR<URoom>& room)
{
	RUNTIME_TRACE_SCOPE(URoomManager, AddRoom, GetRuntimeTraceID());
	if (!room)
		return false;

	const uint64 roomID{ room->GetRoomID() };
	if (roomID == 0)
		return false;

	return _RoomContainer.insert(std::make_pair(roomID, room)).second;
}

// 방 ID로 등록된 방을 잠금 없이 O(1) 조회한다.
SHPTR<URoom> URoomManager::FindRoom(const uint64 roomID) const
{
	RUNTIME_TRACE_SCOPE(URoomManager, FindRoom, GetRuntimeTraceID());
	if (roomID == 0)
		return nullptr;

	const auto it = _RoomContainer.find(roomID);
	return it != _RoomContainer.end() ? it->second : nullptr;
}

// 등록된 모든 방을 RoomID 오름차순 스냅샷으로 수집한다(잠금 0개).
VECTOR<SHPTR<URoom>> URoomManager::SnapshotRooms() const
{
	RUNTIME_TRACE_SCOPE(URoomManager, SnapshotRooms, GetRuntimeTraceID());
	VECTOR<SHPTR<URoom>> rooms;
	rooms.reserve(_RoomContainer.size());
	for (const auto& entry : _RoomContainer) {
		if (entry.second)
			rooms.push_back(entry.second);
	}
	std::sort(rooms.begin(), rooms.end(), [](const SHPTR<URoom>& left, const SHPTR<URoom>& right) {
		return left->GetRoomID() < right->GetRoomID();
	});
	return rooms;
}

// 서버 종료 정리 단계에서 등록된 모든 방 참조를 비운다.
void URoomManager::ClearRooms()
{
	RUNTIME_TRACE_SCOPE(URoomManager, ClearRooms, GetRuntimeTraceID());
	_RoomContainer.clear();
}

SPACE_T
