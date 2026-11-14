#include "CoreDefine.h"
#include "URoom.h"
#include <boost/asio/post.hpp>
#include <utility>

SPACE_H(Core)

URoom::URoom(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context, uint32 maxSessionCount)
    : UObject{ coreInstance }, _MaxSessionCount{ maxSessionCount }, _Strand{ ASIO::make_strand(context) }
{
}

URoom::~URoom() = default;

bool URoom::RequestAddSession(const SHPTR<USession>& session, FUNC<void(bool)> onComplete)
{
    const auto room = ThisWeak<URoom>().lock();
    if (!room || !session || !onComplete)
        return false;

    // 참가 목록은 이 strand에서만 사용하므로 정원 확인과 추가 사이에 다른 입장이 끼어들지 않는다.
    ASIO::post(_Strand, [room, session, onComplete = std::move(onComplete)] {
        const bool added = room->_SessionContainer.size() < room->_MaxSessionCount
            && room->_SessionContainer.insert(WKPTR<USession>{ session }).second;
        onComplete(added);
    });
    return true;
}

void URoom::RequestRemoveSession(const SHPTR<USession>& session)
{
    if (!session)
        return;

    // 집합은 제거할 때 해시를 다시 계산하므로, 게시한 작업이 Session을 붙잡아 WKPTR이 만료되지 않게 한다.
    ASIO::post(_Strand, [room = ThisShared<URoom>(), session] {
        room->_SessionContainer.erase(WKPTR<USession>{ session });
    });
}

SPACE_T
