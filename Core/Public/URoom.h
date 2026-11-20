#pragma once

#include "CoreDefine.h"
#include "UObject.h"
#include "Packet.h"
#include <boost/asio/strand.hpp>

SPACE_H(Core)

class UActor;

// 액터(UActor)들이 머무는 추상 공간의 기반 클래스다.
// 방 내부 상태(_ActorContainer·_ActorIndexMap)와 파생 방의 도메인 상태는 모두 이 Room의 strand에서만
// 읽고 바꾸므로 방 내부에 별도 mutex를 두지 않으며, 다른 Room의 작업과는 동시에 실행될 수 있다.
class CORE_DLL URoom abstract : public UObject {
public:
    using ACTOR_CONTAINER = VECTOR<SHPTR<UActor>>;

protected:
    // context는 Room의 strand가 작업을 실행할 io_context이며 Room과 게시한 작업이 끝날 때까지 유지해야 한다.
    // roomID는 0이 아닌 고유 방 식별자다.
    explicit URoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint64 roomID, uint32 maxActorCount);

public:
    virtual ~URoom() override;
    NO_COPY(URoom)

    // 생성 시 확정된 방 식별자다. 어느 스레드에서나 잠금 없이 읽을 수 있다.
    uint64 GetRoomID() const noexcept { return _RoomID; }

    // 생성 시 지정된 방의 최대 액터 정원이다. 어느 스레드에서나 잠금 없이 읽을 수 있다.
    uint32 GetMaxActorCount() const noexcept { return _MaxActorCount; }

    // 어느 스레드에서나 호출할 수 있다. true는 게시 성공이며 정원·중복·CanEnter 판정과 입장 결과는
    // onComplete(선택)로 이 Room의 strand에서 전달한다.
    // Room이 shared_ptr 소유가 아니거나 actor가 비어 있으면 false다.
    // io_context가 종료되거나 중지되면 onComplete가 실행되지 않을 수 있다. 게시 예외는 호출자에게 전달한다.
    bool RequestEnter(const SHPTR<UActor>& actor, FUNC<void(bool)> onComplete = nullptr);

    // 어느 스레드에서나 호출할 수 있다. 목록 제거 → UActor::ClearRoomID(_RoomID) → OnActorLeft 순서로
    // 이 Room의 strand에서 실행된다. actor가 비어 있거나 목록에 없으면 아무것도 하지 않는다.
    // Room이 shared_ptr 소유가 아니면 std::bad_weak_ptr을 던지며, 게시 예외는 호출자에게 전달한다.
    void RequestLeave(const SHPTR<UActor>& actor);

    // 어느 스레드에서나 호출할 수 있다. 이 Room의 strand에서 현재 참가 중인 통신 액터(GetSession() != nullptr)에
    // 송신을 요청한다. data와 packetSize는 헤더를 제외한 본문과 본문 크기이며 호출 중에 복사한다.
    // packetSize가 PACKET_MAX_BODY_SIZE를 넘거나 본문이 있는데 data가 비어 있으면 보내지 않는다.
    // Room이 shared_ptr 소유가 아니면 std::bad_weak_ptr을 던지며, 게시 예외는 호출자에게 전달한다.
    void RequestBroadcast(const void* data, uint64 packetSize, uint16 packetType);

protected:
    // 어느 스레드에서나 호출할 수 있다. task를 이 Room의 strand에 게시하며, 실행이 끝날 때까지 Room을 유지한다.
    // Room이 shared_ptr 소유가 아니거나 task가 비어 있으면 false다. 게시 예외는 호출자에게 전달한다.
    bool PostToRoom(FUNC<void()> task);

    // 이 Room의 strand에서만 호출한다. 정원·중복·CanEnter를 검사해 통과하면 _ActorContainer에 등록하고
    // actor->SetRoomID(_RoomID)와 OnActorEntered를 실행한 뒤 true를 돌려준다.
    bool TryEnterOnStrand(const SHPTR<UActor>& actor);

    // 이 Room의 strand에서만 호출한다. actor를 _ActorContainer에서 O(1) swap-and-pop으로 제거하고,
    // actor->ClearRoomID(_RoomID) 후 OnActorLeft를 호출한다. 목록에 없었으면 false다.
    bool LeaveOnStrand(const SHPTR<UActor>& actor);

    // 이 Room의 strand에서만 호출한다. 반환한 참조는 현재 strand 작업 동안만 사용한다.
    const ACTOR_CONTAINER& GetActorContainer() const noexcept { return _ActorContainer; }

    // 이 Room의 strand에서만 호출한다. 현재 참가 중인 액터 수다.
    uint64 GetActorCount() const noexcept { return static_cast<uint64>(_ActorContainer.size()); }

    // 이 Room의 strand에서만 호출한다. 해당 액터가 현재 이 방에 속해 있는지 O(1)로 확인한다.
    bool HasActor(const UActor* actor) const noexcept;

    // 이 Room의 strand에서만 호출한다. 현재 참가 중인 모든 통신 액터의 세션에 송신을 요청한다.
    // 내부에서 헤더+본문 불변 프레임을 한 번만 만들어 모든 세션이 공유한다.
    void BroadcastOnStrand(const void* data, uint64 packetSize, uint16 packetType) const;

    // 이 Room의 strand에서만 호출한다. 이미 조립된 불변 송신 프레임을 현재 참가 중인 통신 액터들에게 공유 송신한다.
    // excludedActor가 지정되면 해당 액터를 제외한다.
    void BroadcastSharedOnStrand(const SHPTR<const BUFFER>& framedBuffer, const UActor* excludedActor = nullptr) const;

    // 이 Room의 strand에서 TryEnterOnStrand의 정원·중복 검사 뒤에 호출된다.
    // 파생 방이 입장 가능 조건을 추가 검사할 때 재정의한다. 기본값은 true다.
    virtual bool CanEnter(const SHPTR<UActor>& actor) const;

    // 이 Room의 strand에서 액터가 목록에 추가되고 RoomID가 기록된 직후 호출된다.
    virtual void OnActorEntered(const SHPTR<UActor>& actor);

    // 이 Room의 strand에서 액터가 목록에서 제거되고 ClearRoomID가 끝난 직후 호출된다.
    virtual void OnActorLeft(const SHPTR<UActor>& actor);

private:
    using ACTOR_INDEX_MAP = UNORMAP<const UActor*, uint64>;

    uint64 _RoomID{ 0 };
    uint32 _MaxActorCount{ 0 };
    ASIO::strand<IOCONTEXT::executor_type> _Strand;
    ACTOR_CONTAINER _ActorContainer; // 이 Room의 strand에서만 접근한다.
    ACTOR_INDEX_MAP _ActorIndexMap;  // 이 Room의 strand에서만 접근한다.
};

SPACE_T
