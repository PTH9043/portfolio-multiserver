#include "CoreDefine.h"
#include "URoom.h"
#include "UActor.h"
#include "USession.h"
#include <boost/asio/post.hpp>
#include <cstring>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/URoom.cpp"

SPACE_H(Core)

URoom::URoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint64 roomID, uint32 maxActorCount)
    : UObject{ std::move(coreInstance) },
      _RoomID{ roomID },
      _MaxActorCount{ maxActorCount },
      _Strand{ ASIO::make_strand(context) }
{
    RUNTIME_TRACE_EVENT(Create, URoom, URoom, GetRuntimeTraceID());
}

URoom::~URoom()
{
    RUNTIME_TRACE_EVENT(Destroy, URoom, ~URoom, GetRuntimeTraceID());
}

bool URoom::RequestEnter(const SHPTR<UActor>& actor, FUNC<void(bool)> onComplete)
{
    RUNTIME_TRACE_SCOPE(URoom, RequestEnter, GetRuntimeTraceID());
    const auto room = ThisWeak<URoom>().lock();
    if (!room || !actor)
        return false;

    const auto submission = RUNTIME_TRACE_EVENT(Submit, URoom, RequestEnter, GetRuntimeTraceID());
    ASIO::post(_Strand, [room, actor, onComplete = std::move(onComplete), submission]() mutable {
        RUNTIME_TRACE_CALLBACK(URoom, RequestEnter, room->GetRuntimeTraceID(), submission);
        const bool entered = room->TryEnterOnStrand(actor);
        if (onComplete)
            onComplete(entered);
    });
    return true;
}

void URoom::RequestLeave(const SHPTR<UActor>& actor)
{
    RUNTIME_TRACE_SCOPE(URoom, RequestLeave, GetRuntimeTraceID());
    if (!actor)
        return;

    const auto submission = RUNTIME_TRACE_EVENT(Submit, URoom, RequestLeave, GetRuntimeTraceID());
    ASIO::post(_Strand, [room = ThisShared<URoom>(), actor, submission] {
        RUNTIME_TRACE_CALLBACK(URoom, RequestLeave, room->GetRuntimeTraceID(), submission);
        room->LeaveOnStrand(actor);
    });
}

void URoom::RequestBroadcast(const void* data, uint64 packetSize, uint16 packetType)
{
    RUNTIME_TRACE_SCOPE(URoom, RequestBroadcast, GetRuntimeTraceID());
    const auto framedBuffer = USession::MakePacketBuffer(data, packetSize, packetType);
    if (!framedBuffer)
        return;

    const auto submission = RUNTIME_TRACE_EVENT(Submit, URoom, RequestBroadcast, GetRuntimeTraceID());
    ASIO::post(_Strand, [room = ThisShared<URoom>(), framedBuffer, submission] {
        RUNTIME_TRACE_CALLBACK(URoom, RequestBroadcast, room->GetRuntimeTraceID(), submission);
        room->BroadcastSharedOnStrand(framedBuffer);
    });
}

bool URoom::PostToRoom(FUNC<void()> task)
{
    RUNTIME_TRACE_SCOPE(URoom, PostToRoom, GetRuntimeTraceID());
    const auto room = ThisWeak<URoom>().lock();
    if (!room || !task)
        return false;

    const auto submission = RUNTIME_TRACE_EVENT(Submit, URoom, PostToRoom, GetRuntimeTraceID());
    ASIO::post(_Strand, [room, task = std::move(task), submission]() mutable {
        RUNTIME_TRACE_CALLBACK(URoom, PostToRoom, room->GetRuntimeTraceID(), submission);
        task();
    });
    return true;
}

bool URoom::TryEnterOnStrand(const SHPTR<UActor>& actor)
{
    RUNTIME_TRACE_SCOPE(URoom, TryEnterOnStrand, GetRuntimeTraceID());
    if (!actor)
        return false;
    if (_ActorIndexMap.contains(actor.get()))
        return false;
    if (_ActorContainer.size() >= _MaxActorCount)
        return false;
    if (!CanEnter(actor))
        return false;

    // 벡터 공간을 먼저 확보하고 인덱스 맵을 갱신한 뒤 push_back해 할당 예외 시 일관성을 유지한다.
    _ActorContainer.reserve(_ActorContainer.size() + 1);
    const uint64 index{ static_cast<uint64>(_ActorContainer.size()) };
    _ActorIndexMap.emplace(actor.get(), index);
    _ActorContainer.push_back(actor);
    actor->SetRoomID(_RoomID);
    OnActorEntered(actor);
    return true;
}

bool URoom::LeaveOnStrand(const SHPTR<UActor>& actor)
{
    RUNTIME_TRACE_SCOPE(URoom, LeaveOnStrand, GetRuntimeTraceID());
    if (!actor)
        return false;

    const auto it = _ActorIndexMap.find(actor.get());
    if (it == _ActorIndexMap.end())
        return false;

    const uint64 removeIndex{ it->second };
    const uint64 lastIndex{ static_cast<uint64>(_ActorContainer.size() - 1) };
    // OnActorLeft 실행 중에도 액터 수명을 유지하고, 목록에서는 먼저 제거해 퇴장 브로드캐스트 대상에서 제외한다.
    const SHPTR<UActor> removedActor = _ActorContainer[removeIndex];
    if (removeIndex != lastIndex) {
        _ActorContainer[removeIndex] = std::move(_ActorContainer[lastIndex]);
        _ActorIndexMap[_ActorContainer[removeIndex].get()] = removeIndex;
    }
    _ActorContainer.pop_back();
    _ActorIndexMap.erase(it);

    removedActor->ClearRoomID(_RoomID);
    OnActorLeft(removedActor);
    return true;
}

bool URoom::HasActor(const UActor* actor) const noexcept
{
    if (actor == nullptr)
        return false;
    return _ActorIndexMap.contains(actor);
}

void URoom::BroadcastOnStrand(const void* data, uint64 packetSize, uint16 packetType) const
{
    RUNTIME_TRACE_SCOPE(URoom, BroadcastOnStrand, GetRuntimeTraceID());
    const auto framedBuffer = USession::MakePacketBuffer(data, packetSize, packetType);
    if (!framedBuffer)
        return;

    BroadcastSharedOnStrand(framedBuffer);
}

void URoom::BroadcastSharedOnStrand(const SHPTR<const BUFFER>& framedBuffer, const UActor* excludedActor) const
{
    RUNTIME_TRACE_SCOPE(URoom, BroadcastSharedOnStrand, GetRuntimeTraceID());
    if (!framedBuffer || framedBuffer->empty())
        return;

    for (const auto& actor : _ActorContainer) {
        if (!actor || actor.get() == excludedActor)
            continue;
        if (const auto session = actor->GetSession())
            session->SendShared(framedBuffer);
    }
}

bool URoom::CanEnter(const SHPTR<UActor>& actor) const
{
    (void)actor;
    return true;
}

void URoom::OnActorEntered(const SHPTR<UActor>& actor)
{
    (void)actor;
}

void URoom::OnActorLeft(const SHPTR<UActor>& actor)
{
    (void)actor;
}

SPACE_T
