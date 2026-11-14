#pragma once

#include "UObject.h"
#include <exception>

SPACE_H(Core)

// 워커들과 오류 전달이 모두 끝날 때까지 데이터와 참조 대상을 유지해야 한다.
struct FNetworkWorkerData {
	IOCONTEXT* Context;
	FUNC<void(EXPECTION_PTR)> OnFailure;
	FUNC<void()> OnComplete;
};

class CORE_DLL UNetworkManager final : public UObject {
public:
	explicit UNetworkManager(SHPTR<UCoreInstance> coreInstance);
	virtual ~UNetworkManager() override;
	NO_COPY(UNetworkManager);

	// 반환한 참조를 사용하는 동안 매니저가 살아 있어야 한다.
	IOCONTEXT& GetIOContext() noexcept { return _IOContext; }

	// 서버 워크 스레드
	static void RunWorker(void* Data);
	// guard만 해제한다. 진행 중인 작업을 취소하지 않는다.
	// 예외를 던지지 않는다. guard 자체는 동시 호출을 보호하지 않으므로 여러 스레드에서 동시에 호출하지 않는다.
	void ReleaseWork() noexcept;
private:
	IOCONTEXT								_IOContext;
	WORK_GUARD<IOCONTEXT::executor_type>	_WorkGuard;
};

SPACE_T
