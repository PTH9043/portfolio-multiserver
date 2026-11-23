#include "CoreDefine.h"
#include "URoom.h"
#include "UActor.h"
#include "USession.h"
#include <boost/asio/post.hpp>
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

// 액터 입장을 방 strand에 게시하고 결과를 onComplete로 알린다.
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

// 액터 퇴장을 방 strand에 게시한다.
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

// 액터 퇴장을 방 strand에 게시하고 완료 여부를 onComplete로 전달한다(방 이동 직렬화용).
bool URoom::RequestLeaveWithCallback(const SHPTR<UActor>& actor, FUNC<void(bool)> onComplete)
{
    RUNTIME_TRACE_SCOPE(URoom, RequestLeaveWithCallback, GetRuntimeTraceID());
    const auto room = ThisWeak<URoom>().lock();
    if (!room || !actor)
        return false;

    const auto submission = RUNTIME_TRACE_EVENT(Submit, URoom, RequestLeaveWithCallback, GetRuntimeTraceID());
    ASIO::post(_Strand, [room, actor, onComplete = std::move(onComplete), submission]() mutable {
        RUNTIME_TRACE_CALLBACK(URoom, RequestLeaveWithCallback, room->GetRuntimeTraceID(), submission);
        const bool left = room->LeaveOnStrand(actor);
        if (onComplete)
            onComplete(left);
    });
    return true;
}

// 임의 작업을 이 방의 strand로 직렬화 게시한다(실행 중 방 수명 유지).
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

// 방 strand 안에서 중복·정원을 검사한 뒤 액터를 O(1) 등록하고, 통신 액터는 브로드캐스트 대상 목록에 분리 등록한다.
bool URoom::TryEnterOnStrand(const SHPTR<UActor>& actor)
{
    RUNTIME_TRACE_SCOPE(URoom, TryEnterOnStrand, GetRuntimeTraceID());
    if (!actor)
        return false;
    if (_ActorIndexMap.contains(actor.get()))
        return false;
    if (_ActorContainer.size() >= _MaxActorCount)
        return false;

    SHPTR<USession> session = actor->GetSession();

    // 모든 컨테이너 공간을 먼저 확보하고 인덱스 맵을 갱신해 중간 할당 예외 시 컨테이너 간 불일치를 막는다.
    _ActorContainer.reserve(_ActorContainer.size() + 1);
    if (session)
        _BroadcastTargets.reserve(_BroadcastTargets.size() + 1);

    const uint64 actorIndex{ static_cast<uint64>(_ActorContainer.size()) };
    const auto [actorIt, actorInserted] = _ActorIndexMap.emplace(actor.get(), actorIndex);
    (void)actorInserted;

    if (session) {
        try {
            const uint64 targetIndex{ static_cast<uint64>(_BroadcastTargets.size()) };
            _BroadcastTargetIndexMap.emplace(actor.get(), targetIndex);
        }
        catch (...) {
            _ActorIndexMap.erase(actorIt);
            throw;
        }
        _BroadcastTargets.push_back(FBroadcastTarget{ actor.get(), std::move(session) });
    }

    _ActorContainer.push_back(actor);
    _PublishedActorCount.store(static_cast<uint64>(_ActorContainer.size()), std::memory_order_relaxed);
    actor->SetRoomID(_RoomID);
    OnActorEntered(actor);
    return true;
}

// 방 strand 안에서 swap-and-pop으로 액터와 브로드캐스트 대상을 O(1) 제거하고 퇴장 훅을 호출한다.
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

    const auto targetIt = _BroadcastTargetIndexMap.find(removedActor.get());
    if (targetIt != _BroadcastTargetIndexMap.end()) {
        const uint64 removeTargetIndex{ targetIt->second };
        const uint64 lastTargetIndex{ static_cast<uint64>(_BroadcastTargets.size() - 1) };
        if (removeTargetIndex != lastTargetIndex) {
            _BroadcastTargets[removeTargetIndex] = std::move(_BroadcastTargets[lastTargetIndex]);
            _BroadcastTargetIndexMap[_BroadcastTargets[removeTargetIndex].Actor] = removeTargetIndex;
        }
        _BroadcastTargets.pop_back();
        _BroadcastTargetIndexMap.erase(targetIt);
    }

    _PublishedActorCount.store(static_cast<uint64>(_ActorContainer.size()), std::memory_order_relaxed);

    removedActor->ClearRoomID(_RoomID);
    OnActorLeft(removedActor);
    return true;
}

// 방 strand 안에서 해당 액터가 현재 소속돼 있는지 O(1) 확인한다.
bool URoom::HasActor(const UActor* actor) const noexcept
{
    if (actor == nullptr)
        return false;
    return _ActorIndexMap.contains(actor);
}

// 원시 본문을 1회 프레임화해 방 전체 액터에게 전송한다.
void URoom::BroadcastOnStrand(const void* data, uint64 packetSize, uint16 packetType) const
{
    RUNTIME_TRACE_SCOPE(URoom, BroadcastOnStrand, GetRuntimeTraceID());
    const auto framedBuffer = USession::MakePacketBuffer(data, packetSize, packetType);
    if (!framedBuffer)
        return;

    BroadcastSharedOnStrand(framedBuffer);
}

// 미리 조립된 불변 프레임을 excludedActor를 제외한 방 내 모든 통신 액터(USession)에만 추가 복사 없이 송신한다.
void URoom::BroadcastSharedOnStrand(const SHPTR<const BUFFER>& framedBuffer, const UActor* excludedActor) const
{
    RUNTIME_TRACE_SCOPE(URoom, BroadcastSharedOnStrand, GetRuntimeTraceID());
    if (!framedBuffer || framedBuffer->empty())
        return;

    for (const auto& target : _BroadcastTargets) {
        if (target.Actor == excludedActor)
            continue;
        target.Session->SendShared(framedBuffer);
    }
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
