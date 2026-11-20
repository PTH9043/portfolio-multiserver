#ifdef _WIN32
#include "CoreDefine.h"
#endif
#include "UServerInput.h"

#include <cerrno>
#include <string.h>

#ifdef _WIN32
#include <Windows.h>
#else
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>
#endif

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Private/UServerInput.cpp"

SPACE_H(Server)

namespace {
#ifdef _WIN32
// 인자를 생략하면 직전 Win32 호출의 오류 코드를 사용한다.
Core::ERROR_CODE SystemError(const DWORD error = ::GetLastError()) noexcept
{
	return { static_cast<int>(error), Core::system_category() };
}
#else
// 인자를 생략하면 직전 호출의 errno를 사용한다.
Core::ERROR_CODE SystemError(const int error = errno) noexcept
{
	return { error, Core::generic_category() };
}
#endif
}

struct UServerInput::FImpl {
	Core::ERROR_CODE LastError;
#ifdef _WIN32
	HANDLE StopEvent = nullptr;
#else
	int StopFd = -1;
#endif
};

UServerInput::UServerInput() : _Impl(new FImpl{})
{
	RUNTIME_TRACE_EVENT(Create, UServerInput, UServerInput, _RuntimeTraceID.Get());
#ifdef _WIN32
	_Impl->StopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!_Impl->StopEvent)
		_Impl->LastError = SystemError();
#else
	_Impl->StopFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (_Impl->StopFd == -1)
		_Impl->LastError = SystemError();
#endif
}

UServerInput::~UServerInput()
{
	RUNTIME_TRACE_SCOPE(UServerInput, ~UServerInput, _RuntimeTraceID.Get());
#ifdef _WIN32
	if (_Impl->StopEvent)
		CloseHandle(_Impl->StopEvent);
#else
	if (_Impl->StopFd != -1)
		close(_Impl->StopFd);
#endif
	RUNTIME_TRACE_EVENT(Destroy, UServerInput, ~UServerInput, _RuntimeTraceID.Get());
}

bool UServerInput::IsReady() const noexcept
{
#ifdef _WIN32
	return _Impl->StopEvent != nullptr;
#else
	return _Impl->StopFd != -1;
#endif
}

UServerInput::EResult UServerInput::Wait() noexcept
{
	RUNTIME_TRACE_SCOPE(UServerInput, Wait, _RuntimeTraceID.Get());
	if (!IsReady()) return EResult::Error;
	_Impl->LastError.clear();
#ifdef _WIN32
	HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
	if (input == INVALID_HANDLE_VALUE)
		return Fail(SystemError());
	// 표준 입력 없이 시작한 프로세스는 NULL을 받는다. 받을 입력이 없으므로 입력 끝으로 본다.
	if (!input)
		return EResult::EndOfInput;
	DWORD mode = 0;
	const bool console = GetConsoleMode(input, &mode) != 0;
	const DWORD type = GetFileType(input);
	// 콘솔이 아닌 문자 장치(NUL, 직렬 포트 등)는 동기 읽기 중에 종료 요청으로 깨울 수 없으므로 읽지 않는다.
	// NUL은 읽어도 바로 입력 끝이므로 Unix의 /dev/null처럼 입력 끝으로 본다.
	if (!console && type == FILE_TYPE_CHAR)
		return EResult::EndOfInput;
	if (!console && type != FILE_TYPE_DISK && type != FILE_TYPE_PIPE)
		return Fail(SystemError(ERROR_INVALID_HANDLE));
	return console ? WaitConsole(input) : WaitRedirected(input, type);
#else
	return WaitUnix();
#endif
}

bool UServerInput::RequestStop() noexcept
{
	RUNTIME_TRACE_SCOPE(UServerInput, RequestStop, _RuntimeTraceID.Get());
	if (!IsReady()) return false;
#ifdef _WIN32
	return SetEvent(_Impl->StopEvent) != 0;
#else
	const Core::uint64 value = 1;
	ssize_t result;
	do { result = write(_Impl->StopFd, &value, sizeof(value)); } while (result == -1 && errno == EINTR);
	return result == static_cast<ssize_t>(sizeof(value)) || (result == -1 && errno == EAGAIN);
#endif
}

Core::ERROR_CODE UServerInput::GetLastError() const noexcept { return _Impl->LastError; }

UServerInput::EResult UServerInput::Fail(const Core::ERROR_CODE error) noexcept
{
	RUNTIME_TRACE_SCOPE(UServerInput, Fail, _RuntimeTraceID.Get());
	_Impl->LastError = error;
	return EResult::Error;
}

#ifdef _WIN32
UServerInput::EResult UServerInput::WaitConsole(void* input) noexcept
{
	RUNTIME_TRACE_SCOPE(UServerInput, WaitConsole, _RuntimeTraceID.Get());
	for (;;) {
		HANDLE handles[] = { _Impl->StopEvent, input };
		DWORD wait = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
		if (wait == WAIT_OBJECT_0) return EResult::StopRequested;
		if (wait != WAIT_OBJECT_0 + 1) return Fail(SystemError());
		INPUT_RECORD record{}; DWORD count = 0;
		if (!ReadConsoleInputW(input, &record, 1, &count)) return Fail(SystemError());
		if (count == 0 || record.EventType != KEY_EVENT)
			continue;

		const KEY_EVENT_RECORD& key = record.Event.KeyEvent;
		if (key.bKeyDown && key.wVirtualKeyCode == VK_RETURN)
			return EResult::Enter;
	}
}

UServerInput::EResult UServerInput::WaitRedirected(void* input, unsigned long type) noexcept
{
	RUNTIME_TRACE_SCOPE(UServerInput, WaitRedirected, _RuntimeTraceID.Get());
	for (;;) {
		DWORD stop = WaitForSingleObject(_Impl->StopEvent, 0);
		if (stop == WAIT_OBJECT_0) return EResult::StopRequested;
		if (stop == WAIT_FAILED) return Fail(SystemError());
		if (type == FILE_TYPE_PIPE) {
			DWORD available = 0;
			if (!PeekNamedPipe(input, nullptr, 0, nullptr, &available, nullptr)) {
				DWORD error = ::GetLastError();
				if (error == ERROR_BROKEN_PIPE) return EResult::EndOfInput;
				return Fail(SystemError(error));
			}
			if (available == 0) {
				DWORD wait = WaitForSingleObject(_Impl->StopEvent, 50);
				if (wait == WAIT_OBJECT_0) return EResult::StopRequested;
				if (wait == WAIT_FAILED) return Fail(SystemError());
				continue;
			}
		}
		char buffer[128]; DWORD count = 0;
		if (!ReadFile(input, buffer, sizeof(buffer), &count, nullptr)) {
			DWORD error = ::GetLastError();
			if (error == ERROR_BROKEN_PIPE || error == ERROR_HANDLE_EOF) return EResult::EndOfInput;
			return Fail(SystemError(error));
		}
		if (count == 0) return EResult::EndOfInput;
		if (memchr(buffer, '\n', count)) return EResult::Enter;
	}
}
#else
UServerInput::EResult UServerInput::WaitUnix() noexcept
{
	RUNTIME_TRACE_SCOPE(UServerInput, WaitUnix, _RuntimeTraceID.Get());
	for (;;) {
		pollfd fds[] = { { _Impl->StopFd, POLLIN, 0 }, { STDIN_FILENO, POLLIN, 0 } };
		int result = poll(fds, 2, -1);
		if (result == -1) {
			if (errno == EINTR) continue;
			return Fail(SystemError());
		}
		if (fds[0].revents & POLLIN) return EResult::StopRequested;
		if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) return Fail(SystemError(EIO));
		// 표준 입력이 닫혀 있으면 받을 입력이 없으므로 Windows의 표준 입력 없음과 같이 입력 끝으로 본다.
		if (fds[1].revents & POLLNVAL) return EResult::EndOfInput;
		if (fds[1].revents & (POLLIN | POLLHUP)) {
			char buffer[128]; ssize_t count;
			do { count = read(STDIN_FILENO, buffer, sizeof(buffer)); } while (count == -1 && errno == EINTR);
			if (count == 0) return EResult::EndOfInput;
			if (count == -1) return Fail(SystemError());
			if (memchr(buffer, '\n', static_cast<size_t>(count))) return EResult::Enter;
		}
		else if (fds[1].revents & POLLERR) return Fail(SystemError(EIO));
	}
}
#endif
SPACE_T
