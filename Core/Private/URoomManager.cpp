#include "CoreDefine.h"
#include "URoomManager.h"
#include "URoom.h"

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

SHPTR<URoom> URoomManager::FindRoom(const uint64 roomID) const
{
	RUNTIME_TRACE_SCOPE(URoomManager, FindRoom, GetRuntimeTraceID());
	if (roomID == 0)
		return nullptr;

	const auto it = _RoomContainer.find(roomID);
	return it != _RoomContainer.end() ? it->second : nullptr;
}

void URoomManager::ClearRooms()
{
	RUNTIME_TRACE_SCOPE(URoomManager, ClearRooms, GetRuntimeTraceID());
	_RoomContainer.clear();
}

uint64 URoomManager::GetRoomCount() const noexcept
{
	RUNTIME_TRACE_SCOPE(URoomManager, GetRoomCount, GetRuntimeTraceID());
	return static_cast<uint64>(_RoomContainer.size());
}

SPACE_T
