#include "CoreDefine.h"
#include "UThreadManager.h"

SPACE_H(Core)

UThreadManager::UThreadManager(SHPTR<UCoreInstance> coreInstance)
    : UObject{ coreInstance }
{}

UThreadManager::~UThreadManager()
{
	// 실행을 기다리는 스레드는 작업을 실행하지 않고 종료한다.
	ReleaseWaitingThreads(EStartSignal::Cancel);

	// 이미 실행 중인 작업의 종료 요청은 각 서비스가 담당한다.
	if (!JoinStartedThreads())
		std::terminate();
}

bool UThreadManager::RegisterFunc(THREADFUNC Work, void* Data)
{
	if (!Work || _StartSignal.load(std::memory_order_acquire) != EStartSignal::Waiting)
		return false;

	try {
		_Threads.emplace_back([this, Work = std::move(Work), Data]() {
			_StartSignal.wait(EStartSignal::Waiting, std::memory_order_acquire);
			if (_StartSignal.load(std::memory_order_acquire) != EStartSignal::Run)
				return;

			try {
				Work(Data);
			}
			catch (...) {
				// 실행 중 오류 전달은 다음 단계에서 연결한다.
			}
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
	ReleaseWaitingThreads(EStartSignal::Run);

	if (!JoinStartedThreads())
		return false;

	_StartSignal.store(EStartSignal::Waiting, std::memory_order_relaxed);
	return true;
}

void UThreadManager::ReleaseWaitingThreads(const EStartSignal signal) noexcept
{
	// 신호는 등록과 JoinAll을 호출하는 제어 스레드만 바꾼다.
	if (_StartSignal.load(std::memory_order_acquire) != EStartSignal::Waiting)
		return;
	_StartSignal.store(signal, std::memory_order_release);
	_StartSignal.notify_all();
}

bool UThreadManager::JoinStartedThreads()
{
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
