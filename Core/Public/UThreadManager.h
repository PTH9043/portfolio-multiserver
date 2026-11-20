#pragma once

#include "UObject.h"

SPACE_H(Core)

// 등록된 작업을 일괄 실행하고 스레드의 종료를 기다린다.
class CORE_DLL UThreadManager final : public UObject {
public:
	explicit UThreadManager(SHPTR<UCoreInstance> coreInstance);
	virtual ~UThreadManager() override;
	NO_COPY(UThreadManager);

	// 등록과 JoinAll은 한 제어 스레드에서 호출한다. Data의 수명은 작업 종료까지 유지해야 한다.
	// 스레드를 생성하지만 등록한 함수는 실행 신호를 기다린다.
	// 생성 실패 시 대기 중인 스레드들을 취소하고 false를 반환한다.
	// Work는 오류를 자기 경로로 전달하고 예외를 밖으로 내보내지 않는다. 내보내면 그 예외를 stderr에 남기고
	// 프로그램을 abort로 종료한다.
	bool RegisterFunc(THREADFUNC Work, void* Data);
	// 등록된 스레드에 실행을 허용하고 종료할 때까지 기다린다.
	// 모든 스레드의 종료 대기에 성공하면 true, 대기 실패 시 false를 반환한다.
	// 등록이 취소된 경우에도 스레드가 모두 정리되면 true다.
	bool JoinAll();

private:
	enum class EStartSignal : uint8 { Waiting, Run, Cancel };

	// 실행 신호를 기다리는 스레드가 있으면 Run 또는 Cancel 신호를 보내 깨운다.
	void ReleaseWaitingThreads(EStartSignal signal) noexcept;
	// 생성된 스레드만 기다린다. 등록된 작업을 새로 실행하지 않는다.
	bool JoinStartedThreads();

	THREADS					_Threads;
	ATOMIC<EStartSignal>		_StartSignal{ EStartSignal::Waiting };
};

SPACE_T
