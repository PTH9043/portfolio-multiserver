#pragma once

#include "CoreDefine.h"
#include "UServerInput.h"

SPACE_H(Core)
class UCoreInstance;
SPACE_T

SPACE_H(Server)

using namespace Core;

// 서버의 가장 작은 뼈대다. 패킷과 세션은 아직 없다.
class UServerApp final
{
public:
	UServerApp() = default;
	~UServerApp();
	NO_COPY(UServerApp);

	// 제어 스레드에서 호출한다. Core 준비가 성공하면 true, 이미 시작했거나 실패하면 false.
	bool Start();

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
	SHPTR<UCoreInstance> _CoreInstance;
	UQPTR<UServerInput> _ServerInput;
};

SPACE_T
