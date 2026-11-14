#pragma once

#include "CoreDefine.h"
#include "UObject.h"
#include "USession.h"
#include <boost/asio/strand.hpp>

SPACE_H(Core)

// 방별 정원과 참가 세션의 약한 참조를 보관한다.
// 참가 목록은 이 Room의 strand에서만 읽고 바꾸므로 다른 Room의 작업과는 동시에 실행될 수 있다.
class CORE_DLL URoom final : public UObject {
    using SESSIONCONTAINER = UNORSET<WKPTR<USession>, FHashableHash<WKPTR<USession>>, FHashableEqual<WKPTR<USession>>>;
public:
    // context는 Room의 strand가 작업을 실행할 io_context이며 Room과 게시한 작업이 끝날 때까지 유지해야 한다.
    explicit URoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint32 maxSessionCount);
    virtual ~URoom() override;

    // USession의 입장 처리(RequestEnterRoom → ReserveRoom)에서만 호출한다. 직접 호출하면 Session의 현재 방이
    // 기록되지 않아 Close가 목록 제거를 요청하지 않으며, Session이 소멸한 뒤 만료된 항목이 정원을 차지한다.
    // 어느 스레드에서나 호출할 수 있다. true는 게시 성공이며 정원 확인과 목록 추가 결과는 onComplete로 전달한다.
    // Room이 shared_ptr 소유가 아니거나 session·onComplete가 비어 있으면 false다.
    // onComplete는 이 Room의 strand에서 실행되므로 다른 작업의 완료를 기다리며 막으면 안 된다.
    // Room과 Session은 onComplete 실행까지 유지된다. 추가된 Session은 목록에서 제거될 때까지 살아 있어야 한다.
    // 제거는 USession의 연결 종료 처리가 RequestRemoveSession으로 요청한다. 연결을 유지한 채 방만 나가는 퇴장은 아직 구현되지 않았다.
    // io_context가 종료되거나 중지되면 onComplete가 실행되지 않을 수 있다. 게시 예외는 호출자에게 전달한다.
    // 목록 추가 중 할당 예외는 네트워크 워커의 오류 경로로 전달되며 onComplete는 실행되지 않는다.
    bool RequestAddSession(const SHPTR<USession>& session, FUNC<void(bool)> onComplete);

    // USession의 연결 종료 처리(Close, 수신 종료·오류, 송신 실패)에서만 호출한다.
    // 직접 호출하면 Session의 현재 방 기록은 그대로 남는다.
    // 어느 스레드에서나 호출할 수 있으며 목록 제거는 이 Room의 strand에서 실행된다.
    // 게시한 작업이 Room과 Session을 붙잡으므로 Session이 살아 있는 동안 제거된다.
    // 같은 Session의 추가 요청보다 나중에 게시한 제거 요청은 추가 뒤에 실행된다.
    // session이 비어 있거나 목록에 없으면 아무것도 하지 않는다.
    // Room이 shared_ptr 소유가 아니면 std::bad_weak_ptr을 던지며, 게시 예외는 호출자에게 전달한다.
    void RequestRemoveSession(const SHPTR<USession>& session);

    // 생성 시 지정된 방의 정원이다.
    uint32 GetMaxSessionCount() const noexcept { return _MaxSessionCount; }

private:
    uint32 _MaxSessionCount{};
    ASIO::strand<IOCONTEXT::executor_type> _Strand;
    // 등록된 Session은 집합에서 제거될 때까지 살아 있어야 한다.
    // 만료된 WKPTR을 집합에 남기면 해시가 바뀐다.
    SESSIONCONTAINER _SessionContainer;
};

SPACE_T
