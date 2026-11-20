#pragma once

#include "CoreTypes.h"
#include "CoreMacro.h"
#include "RuntimeTrace.h"

SPACE_H(Server)

// 한 입력 스레드가 Wait을 호출한다. 객체는 Wait이 반환된 뒤 파괴한다.
class UServerInput final {
public:
	enum class EResult { Enter, EndOfInput, StopRequested, Error };

	UServerInput();
	~UServerInput();
	NO_COPY(UServerInput);

	bool IsReady() const noexcept;
	// 입력 스레드에서 호출한다. Enter, 입력 끝(EOF·빈 파일·NUL·표준 입력 없음), 종료 요청, 오류 중 하나로 반환한다.
	// 읽을 수 없는 종류의 표준 입력이면 오류다.
	EResult Wait() noexcept;
	bool RequestStop() noexcept;
	// 생성 직후 또는 Wait 반환 후 제어 스레드에서 확인한다.
	Core::ERROR_CODE GetLastError() const noexcept;

private:
	// 오류 코드를 기록하고 Error를 반환한다.
	EResult Fail(Core::ERROR_CODE error) noexcept;
#ifdef _WIN32
	EResult WaitConsole(void* input) noexcept;
	EResult WaitRedirected(void* input, unsigned long type) noexcept;
#else
	EResult WaitUnix() noexcept;
#endif

	Core::FRuntimeTraceID _RuntimeTraceID;
	struct FImpl;
	Core::UQPTR<FImpl> _Impl;
};

SPACE_T
