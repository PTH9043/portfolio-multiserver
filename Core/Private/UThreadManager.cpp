#include "CoreDefine.h"
#include "UThreadManager.h"
#include <cstdio>
#include <cstdlib>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UThreadManager.cpp"

SPACE_H(Core)

namespace {
// 등록 함수 밖으로 나온 예외로 프로그램이 종료될 때 어떤 예외였는지 남긴다.
// 프로세스가 곧 끝나서 로그 스레드가 출력하지 못하므로 Logger 대신 stderr에 직접 쓴다.
void ReportUnhandledWorkerException() noexcept
{
	const EXPECTION_PTR failure = std::current_exception();
	if (!failure)
		std::fprintf(stderr, "Worker thread terminated without an active exception.\n");
	else {
		try {
			std::rethrow_exception(failure);
		}
		catch (const std::exception& exception) {
			std::fprintf(stderr, "Worker thread terminated by an unhandled exception: %s\n", exception.what());
		}
		catch (...) {
			std::fprintf(stderr, "Worker thread terminated by an unhandled unknown exception.\n");
		}
	}
	std::fflush(stderr);
	// 덤프와 디버거로 발생 지점을 확인할 수 있도록 abort로 끝낸다.
	std::abort();
}
}

UThreadManager::UThreadManager(SHPTR<UCoreInstance> coreInstance)
    : UObject{ coreInstance }
{
	RUNTIME_TRACE_EVENT(Create, UThreadManager, UThreadManager, GetRuntimeTraceID());
}

UThreadManager::~UThreadManager()
{
	RUNTIME_TRACE_SCOPE(UThreadManager, ~UThreadManager, GetRuntimeTraceID());
	// 실행을 기다리는 스레드는 작업을 실행하지 않고 종료한다.
	ReleaseWaitingThreads(EStartSignal::Cancel);

	// 이미 실행 중인 작업의 종료 요청은 각 서비스가 담당한다.
	if (!JoinStartedThreads())
		std::terminate();
	RUNTIME_TRACE_EVENT(Destroy, UThreadManager, ~UThreadManager, GetRuntimeTraceID());
}

bool UThreadManager::RegisterFunc(THREADFUNC Work, void* Data)
{
	RUNTIME_TRACE_SCOPE(UThreadManager, RegisterFunc, GetRuntimeTraceID());
	if (!Work || _StartSignal.load(std::memory_order_acquire) != EStartSignal::Waiting)
		return false;

	try {
		const auto submission = RUNTIME_TRACE_EVENT(Submit, UThreadManager, RegisterFunc, GetRuntimeTraceID());
		_Threads.emplace_back([this, Work = std::move(Work), Data, submission]() {
			const FRuntimeTraceID worker;
			RUNTIME_TRACE_EVENT(Create, THREAD, RegisterFunc, worker.Get());
			RUNTIME_TRACE_CALLBACK(THREAD, RegisterFunc, worker.Get(), submission);
			_StartSignal.wait(EStartSignal::Waiting, std::memory_order_acquire);
			if (_StartSignal.load(std::memory_order_acquire) != EStartSignal::Run) {
				RUNTIME_TRACE_EVENT(Destroy, THREAD, RegisterFunc, worker.Get());
				return;
			}

			// 등록 함수는 자기 오류 경로로 오류를 전달하고 예외를 밖으로 내보내지 않는다.
			// 내보내면 프로그램이 종료되며, 어떤 예외였는지는 종료 처리기가 남긴다.
			// MSVC의 종료 처리기는 스레드마다 따로 설정되므로 이 스레드에서 설정한다.
			std::set_terminate(ReportUnhandledWorkerException);
			Work(Data);
			// 워커 종료 전 스레드 로컬 메모리 캐시를 중앙 풀로 반환해 재사용 경로를 보장한다.
			GThreadMemoryCache.FlushToCentral();
			RUNTIME_TRACE_EVENT(Destroy, THREAD, RegisterFunc, worker.Get());
		});
		return true;
	}
	catch (...) {
		// 이미 생성된 스레드도 등록 함수를 실행하지 않고 종료한다.
		ReleaseWaitingThreads(EStartSignal::Cancel);
		// 정리에 실패하면 남은 스레드와 취소 상태를 유지해 JoinAll에서 다시 기다린다.
		JoinStartedThreads();
		return false;
	}
}

bool UThreadManager::JoinAll()
{
	RUNTIME_TRACE_SCOPE(UThreadManager, JoinAll, GetRuntimeTraceID());
	ReleaseWaitingThreads(EStartSignal::Run);

	if (!JoinStartedThreads())
		return false;

	_StartSignal.store(EStartSignal::Waiting, std::memory_order_relaxed);
	return true;
}

void UThreadManager::ReleaseWaitingThreads(const EStartSignal signal) noexcept
{
	RUNTIME_TRACE_SCOPE(UThreadManager, ReleaseWaitingThreads, GetRuntimeTraceID());
	// 신호는 등록과 JoinAll을 호출하는 제어 스레드만 바꾼다.
	if (_StartSignal.load(std::memory_order_acquire) != EStartSignal::Waiting)
		return;
	_StartSignal.store(signal, std::memory_order_release);
	_StartSignal.notify_all();
}

bool UThreadManager::JoinStartedThreads()
{
	RUNTIME_TRACE_SCOPE(UThreadManager, JoinStartedThreads, GetRuntimeTraceID());
	const THREAD_ID CurrentThreadID = std::this_thread::get_id();
	for (const THREAD& Thread : _Threads) {
		if (Thread.joinable() && Thread.get_id() == CurrentThreadID)
			return false;
	}

	try {
		for (THREAD& Thread : _Threads) {
			if (Thread.joinable())
				Thread.join();
		}
	}
	catch (...) {
		return false;
	}

	_Threads.clear();
	return true;
}

SPACE_T
