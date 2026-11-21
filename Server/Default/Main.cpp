#include "ServerApp.h"

#include <crtdbg.h>
#include <stdio.h>
#include <string.h>

#include "absl/log/globals.h"
#include "absl/log/initialize.h"

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Server/Default/Main.cpp"

namespace {
// 0 이상 maxValue 이하의 10진수만 받는다. 유휴 타임아웃(0=비활성) 파싱에 쓴다.
bool ParseNonNegativeNumber(const char* text, const Core::uint32 maxValue, Core::uint32& result)
{
	if (nullptr == text || '\0' == *text)
		return false;

	Core::uint32 value = 0;
	for (; '\0' != *text; ++text) {
		if (*text < '0' || *text > '9')
			return false;
		const Core::uint32 digit = static_cast<Core::uint32>(*text - '0');
		if (value > (maxValue - digit) / 10)
			return false;
		value = value * 10 + digit;
	}

	result = value;
	return true;
}

// 1 이상 maxValue 이하의 10진수만 받는다. 워커 수와 포트에 쓴다.
bool ParsePositiveNumber(const char* text, const Core::uint32 maxValue, Core::uint32& result)
{
	if (!ParseNonNegativeNumber(text, maxValue, result) || result == 0)
		return false;
	return true;
}

// 워커가 보관한 실패 원인을 출력한다. 원인이 저장되지 않은 실패도 있으므로 비어 있으면 그 사실만 출력한다.
// 워커 종료 뒤에 호출하므로 Logger가 아니라 콘솔에 직접 쓴다.
void PrintFailure(const char* label, const Core::EXPECTION_PTR& failure)
{
	if (!failure) {
		fprintf(stderr, "%s error: no detail.\n", label);
		return;
	}
	try {
		std::rethrow_exception(failure);
	}
	catch (const std::exception& error) {
		fprintf(stderr, "%s error: %s\n", label, error.what());
	}
	catch (...) {
		fprintf(stderr, "%s error: unknown exception.\n", label);
	}
}
}

int main(int argc, char* argv[])
{
	_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);

	// protobuf 내부(absl)는 잘못된 패킷 해석 같은 경고를 Logger를 거치지 않고 stderr에 직접 쓴다.
	// 해석 실패는 UServerSession이 Logger에 Warning으로 남기므로 중복인 absl 출력은 끄고 FATAL만 남긴다.
	// 초기화 전에는 기준과 관계없이 모두 stderr로 나가므로 InitializeLog를 먼저 부른다. (2026-11-20 사용자 선택)
	absl::InitializeLog();
	absl::SetStderrThreshold(absl::LogSeverityAtLeast::kFatal);

	Core::uint32 workerCount = 0;
	Core::uint32 port = 0;
	Core::ERuntimeTraceMode traceMode = Core::ERuntimeTraceMode::AsyncStream;
	Core::MILLISECONDS idleTimeout = Core::SESSION_DEFAULT_IDLE_TIMEOUT;
	if (argc < 3 || argc > 5
		|| !ParsePositiveNumber(argv[1], static_cast<Core::uint32>(-1), workerCount)
		|| !ParsePositiveNumber(argv[2], 65535, port)) {
		fprintf(stderr, "Usage: ServerApp <network-worker-count> <port> [--trace=stream|flight|off] [--idle-timeout-ms=<ms>]\n");
		return 1;
	}
	constexpr const char* IDLE_TIMEOUT_PREFIX = "--idle-timeout-ms=";
	const Core::uint64 idlePrefixLen = static_cast<Core::uint64>(strlen(IDLE_TIMEOUT_PREFIX));
	for (int i = 3; i < argc; ++i) {
		if (strcmp(argv[i], "--trace=stream") == 0)
			traceMode = Core::ERuntimeTraceMode::AsyncStream;
		else if (strcmp(argv[i], "--trace=flight") == 0)
			traceMode = Core::ERuntimeTraceMode::FlightRecorder;
		else if (strcmp(argv[i], "--trace=off") == 0)
			traceMode = Core::ERuntimeTraceMode::Off;
		else if (strncmp(argv[i], IDLE_TIMEOUT_PREFIX, idlePrefixLen) == 0) {
			Core::uint32 idleMs = 0;
			if (!ParseNonNegativeNumber(argv[i] + idlePrefixLen, 3600000, idleMs)) {
				fprintf(stderr, "Usage: ServerApp <network-worker-count> <port> [--trace=stream|flight|off] [--idle-timeout-ms=<ms>]\n");
				return 1;
			}
			idleTimeout = Core::MILLISECONDS{ idleMs };
		}
		else {
			fprintf(stderr, "Usage: ServerApp <network-worker-count> <port> [--trace=stream|flight|off] [--idle-timeout-ms=<ms>]\n");
			return 1;
		}
	}

	Core::FRuntimeTraceRun runtimeTrace{ traceMode };
	RUNTIME_TRACE_SCOPE(ServerApp, main, 0);

	bool success = false;
	{
		Server::UServerApp serverApp;
		if (serverApp.Start(static_cast<Core::uint16>(port), idleTimeout)) {
			if (serverApp.RegisterWorkers(workerCount)) {
				printf("Press Enter to stop the server.\n");
				success = serverApp.JoinWorkers();
				if (!success)
					fprintf(stderr, "Failed to wait for server workers.\n");

				if (serverApp.HasLoggerFailure()) {
					fprintf(stderr, "Logger worker failed.\n");
					PrintFailure("Logger", serverApp.GetLoggerFailure());
					success = false;
				}

				if (serverApp.HasInputFailure()) {
					fprintf(stderr, "Input worker failed: %d\n", serverApp.GetInputFailure().value());
					success = false;
				}
				if (serverApp.HasNetworkFailure()) {
					fprintf(stderr, "Network worker failed.\n");
					PrintFailure("Network", serverApp.GetNetworkFailure());
					success = false;
				}
			}
			else {
				fprintf(stderr, "Failed to register server workers.\n");
			}

			serverApp.Stop();
			if (serverApp.IsRunning())
				success = false;
		}
		else {
			fprintf(stderr, "Server failed to start.\n");
		}
	}

	// 서버 앱과 CoreInstance를 먼저 정리한 뒤 메모리 풀을 정리한다.
	Core::UCoreGlobal::DeleteInstance();
	printf("End Server\n");
	return success ? 0 : 1;
}
