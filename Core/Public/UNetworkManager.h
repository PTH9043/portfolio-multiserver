#pragma once

#include "UObject.h"
#include <exception>
#include <boost/asio/strand.hpp>

SPACE_H(Core)

class UListener;
class USession;

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

	// 네트워크를 종료한다. 부르면 네트워크 워커의 run()이 반드시 반환하도록 만든다. 종료 완료는 기다리지 않는다.
	// 순서: 보관한 리스너 Stop → RequestCloseSessions(이후 등록 요청도 닫힘) → work guard 해제.
	// 세션 닫기 게시가 실패하면 io_context를 stop해 워커를 강제로 끝낸다. 이때 남은 핸들러는 실행되지 않고
	// io_context가 소멸할 때 정리된다. 각 단계의 오류는 Logger에 기록한다.
	// 어느 스레드에서나 호출할 수 있지만 한 번만 호출하며, StartListener와 동시에 호출하지 않는다.
	void Shutdown() noexcept;

	// ServerApp이 만든 리스너(UListener의 파생 객체)를 받아 시작하고, 시작에 성공하면 보관한다.
	// 네트워크 워커 등록 전 제어 스레드에서 한 번 호출한다. 리스너는 이 매니저의 io_context로 만들어야 한다.
	// listener가 비어 있으면 invalid_argument, 이미 보관한 리스너가 있으면 already_started를 error로 돌려준다.
	// 시작 실패 시 시스템 오류는 error로 돌려주고 리스너를 보관하지 않는다.
	// 할당이나 비동기 등록의 예외는 호출자에게 전달한다.
	bool StartListener(SHPTR<UListener> listener, const TCP_ENDPOINT& endpoint,
		FUNC<void(const BOOST_ERROR_CODE&)> onFailure, BOOST_ERROR_CODE& error);

	// 어느 스레드에서나 호출할 수 있다. 목록 변경은 _Strand에서 반환 뒤에 실행된다.
	// 같은 Session의 추가보다 나중에 게시한 제거는 추가 뒤에 실행된다. 그래서 접속 처리에서는 Session의
	// Receive·Close보다 먼저 호출해야 한다. 등록한 Session은 Close로 제거될 때까지 매니저가 유지한다.
	// RequestCloseSessions의 처리 뒤에 실행되는 추가는 목록에 넣지 않고 그 Session에 Close를 요청한다.
	// session이 비어 있으면 아무것도 하지 않는다. 게시 예외는 호출자에게 전달하고,
	// 목록 변경 중 할당 예외는 네트워크 워커의 오류 경로로 전달된다.
	// io_context가 종료되거나 중지되면 실행되지 않을 수 있다.
	void RequestAddSession(const SHPTR<USession>& session);
	// USession의 연결 종료 처리에서 호출한다. 목록에 없으면 아무것도 하지 않는다.
	void RequestRemoveSession(const SHPTR<USession>& session);

	// 어느 스레드에서나 호출할 수 있다. 매니저 strand에서 종료 중으로 표시하고 목록의 각 Session에 Close를 요청한다.
	// 그 뒤에 실행되는 추가 요청의 Session은 목록에 넣지 않고 닫는다. 종료 중 표시는 되돌리지 않는다.
	// 반환은 종료 완료를 뜻하지 않는다. 게시 예외는 호출자에게 전달되며, 처리 중 예외는 네트워크 워커로 전달된다.
	// 한 Session의 Close에서 예외가 나면 순회가 중단되어 나머지 Session에는 Close를 요청하지 못한다.
	// io_context가 종료되거나 중지되면 실행되지 않을 수 있다.
	void RequestCloseSessions();

private:
	// guard만 해제한다. 진행 중인 작업을 취소하지 않는다.
	// 예외를 던지지 않는다. guard 자체는 동시 호출을 보호하지 않으므로 여러 스레드에서 동시에 호출하지 않는다.
	void ReleaseWork() noexcept;

	// 멤버는 선언 역순으로 소멸하므로 세션과 리스너가 _IOContext보다 먼저 정리된다.
	IOCONTEXT								_IOContext;
	WORK_GUARD<IOCONTEXT::executor_type>	_WorkGuard;
	ASIO::strand<IOCONTEXT::executor_type>	_Strand;			// _SessionContainer 전용
	SHPTR<UListener>						_Listener;			// 워커 등록 전 제어 스레드에서만 설정한다.
	UNORSET<SHPTR<USession>>				_SessionContainer;	// _Strand에서만 접근한다.
	bool									_Closing{ false };	// _Strand에서만 접근한다. RequestCloseSessions 처리 뒤 true다.
};

SPACE_T
