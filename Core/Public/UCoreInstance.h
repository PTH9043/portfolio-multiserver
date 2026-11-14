#pragma once
#include "UCoreBase.h"
#include "UNetworkManager.h"

SPACE_H(Core)

class UThreadManager;
class ULogger;

class CORE_DLL UCoreInstance final : public UCoreBase {
public:
    // 제어 스레드에서 한 번 호출하고, 완료된 뒤 다른 스레드에 공개한다.
    // 이미 초기화한 경우 false를 반환한다. 객체 생성 예외는 호출자에게 전달한다.
    bool Initialize();

    // 제어 스레드에서 등록한다. 워커와 오류 전달이 끝날 때까지 이 객체와 콜백 대상을 유지한다.
    // 등록 실패 시 false를 반환한다. 저장 공간과 콜백 생성 예외는 호출자에게 전달한다.
    bool RegisterWorkerThread(const uint32 threadNum, FUNC<void(EXPECTION_PTR)> onFailure, FUNC<void()> onComplete);

    // 초기화 전에는 빈 포인터다. 초기화와 동시에 호출하면 안 된다.
    SHPTR<UThreadManager> GetThreadManager() const noexcept { return _ThreadManager; }
    SHPTR<UNetworkManager> GetNetworkManager() const noexcept { return _NetworkManager; }
    SHPTR<ULogger> GetLogger() const noexcept { return _Logger; }

private:
    SHPTR<UNetworkManager> _NetworkManager;
    SHPTR<ULogger> _Logger;
    LIST<FNetworkWorkerData> _NetworkWorkerData;
    // 기본 멤버 소멸 순서에서 스레드 매니저가 먼저 정리된다.
    SHPTR<UThreadManager> _ThreadManager;
};

SPACE_T
