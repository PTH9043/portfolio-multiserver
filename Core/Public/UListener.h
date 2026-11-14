#pragma once

#include "CoreDefine.h"
#include "UObject.h"

SPACE_H(Core)

// io_context는 리스너와 전달받은 소켓이 파괴될 때까지 유지해야 한다.
class CORE_DLL UListener : public UObject {
public:
    using FOnFailure = FUNC<void(const BOOST_ERROR_CODE&)>;

protected:
    explicit UListener(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context);

public:
    virtual ~UListener() override;
    NO_COPY(UListener);

    // shared_ptr 소유 상태에서 호출한다. 주소와 포트는 호출자가 지정한다.
    // 시스템 오류는 error로 반환하고, 할당이나 비동기 등록의 예외는 호출자에게 전달한다.
    // 워커 실행 전 제어 스레드에서 호출한다. 이 동안 Stop을 포함한 다른 스레드의 접근은 없어야 한다.
    // 시작에 성공한 리스너는 다시 시작하지 않는다.
    bool Start(const TCP_ENDPOINT& endpoint,
        FOnFailure onFailure, BOOST_ERROR_CODE& error);
    // 수락을 취소한다. 이미 완료되어 전달 대기 중이거나 실행 중인 콜백은 기다리지 않는다.
    // 실패 콜백 대상과 파생 리스너가 참조하는 객체는 관련 워커와 완료 핸들러 종료까지 유지해야 한다.
    void Stop(BOOST_ERROR_CODE& error) noexcept;

protected:
    // 수락된 소켓의 소유권을 파생 클래스에 전달한다. 워커에서 잠금 없이 호출한다.
    virtual void HandleAcceptedSocket(TCPSOCKET socket) = 0;

private:
    struct FHandlers {
        FOnFailure OnFailure;
    };

    // 워커 시작 전 또는 _Mutex를 소유한 상태에서 호출한다.
    void BeginAccept();
    void OnAccept(const BOOST_ERROR_CODE& error,
        TCPSOCKET socket);
    // 정리 중 난 오류를 Logger에 기록한다. 오류가 없으면 아무것도 하지 않는다. _Mutex 밖에서 호출한다.
    void LogCleanupError(STRINGVIEW where, const BOOST_ERROR_CODE& error) noexcept;

    SHARED_MUTEX _Mutex;
    TCPACCEPTOR _Acceptor;
    SHPTR<FHandlers> _Handlers;
    bool _Started{ false };
    bool _Running{ false };
};

SPACE_T
