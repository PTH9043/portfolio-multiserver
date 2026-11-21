#pragma once

#include "CoreDefine.h"
#include "USession.h"
#include "UServerInput.h"

SPACE_H(Core)
class UCoreInstance;
SPACE_T

SPACE_H(Server)

class UNicknameRegistry;

using namespace Core;

// 서버 앱이다. 127.0.0.1의 지정 포트에서 접속을 받아 고정 4채널·리스너·워커를 운영한다.
class UServerApp final
{
public:
	UServerApp();
	~UServerApp();
	NO_COPY(UServerApp);

	// 제어 스레드에서 RegisterWorkers보다 먼저 호출한다. Core·고정 방 4개를 준비하고 127.0.0.1:port에서 리스너를 시작한다.
	// sessionIdleTimeout이 0ms이면 유휴 타임아웃을 비활성화한다.
	// 성공하면 true, 이미 시작했거나 준비·리스너 시작에 실패하면 false다. 실패 원인은 콘솔에 쓴다.
	// 리스너의 accept가 오류로 끝나면 Logger에 Error로 남기고 네트워크 실패로 서버 종료를 요청한다.
	bool Start(uint16 port, MILLISECONDS sessionIdleTimeout = SESSION_DEFAULT_IDLE_TIMEOUT);

	// 제어 스레드에서 호출한다. 워커 함수는 JoinWorkers 전까지 실행되지 않는다.
	bool RegisterWorkers(uint32 networkWorkerCount);
	// 등록된 워커의 종료 대기 결과다. 등록 성공 여부와 별도로 확인한다.
	bool JoinWorkers();

	// 제어 스레드에서 호출한다. 정리가 확인되지 않으면 실행 상태를 유지한다.
	// 시작하지 않았으면 아무것도 하지 않는다.
	void Stop();

	// 워커가 호출할 수 있다. 스레드를 join하지 않는다.
	// ServerApp과 Core는 관리되는 모든 호출이 끝날 때까지 유지해야 한다.
	void RequestStop() noexcept;

	bool IsRunning() const { return _bRunning.load(); }
	bool HasNetworkFailure() const noexcept { return _NetworkFailed.load(std::memory_order_acquire); }
	EXPECTION_PTR GetNetworkFailure() const;
	bool HasLoggerFailure() const noexcept { return _LoggerFailed.load(std::memory_order_acquire); }
	EXPECTION_PTR GetLoggerFailure() const;
	bool HasInputFailure() const noexcept { return _InputFailed.load(); }
	ERROR_CODE GetInputFailure() const;

private:
	enum class EStopState { NotRequested, Requesting, Requested, Failed };
	static void RunInputWorker(void* data) noexcept;
	void ReportFailure(ATOMIC<bool>& failed, EXPECTION_PTR& storedFailure, EXPECTION_PTR failure) noexcept;
	void LogProducerFinished() noexcept;
	void ClearObject();

	FRuntimeTraceID _RuntimeTraceID;
	ATOMIC<bool>	_bRunning{ false };
	ATOMIC<EStopState> _StopState{ EStopState::NotRequested };
	ATOMIC<bool>	_NetworkFailed{ false };
	ATOMIC<bool>	_InputFailed{ false };
	ATOMIC<bool>	_LoggerFailed{ false };
	ATOMIC_UINT64 _RemainingLogProducers{ 0 };
	mutable SHARED_MUTEX _FailureMutex;
	EXPECTION_PTR _NetworkFailure;
	EXPECTION_PTR _LoggerFailure;
	ERROR_CODE _InputFailure;
	bool _WorkersRegistered{ false };
	uint16 _ListenPort{ 0 };
	SHPTR<UCoreInstance> _CoreInstance;
	SHPTR<UNicknameRegistry> _NicknameRegistry;
	UQPTR<UServerInput> _ServerInput;
};

SPACE_T
