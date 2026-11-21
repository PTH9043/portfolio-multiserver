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
	Server::FDbConfig dbConfig{};
	const char* usageMsg =
		"Usage: ServerApp <network-worker-count> <port> [--trace=stream|flight|off] "
		"[--idle-timeout-ms=<ms>] [--db=mysql|off] [--db-host=<host>] [--db-port=<port>] "
		"[--db-user=<user>] [--db-pass=<pass>] [--db-name=<name>] [--db-workers=<1..16>] [--pbkdf2-iters=<1000..300000>]\n";
	if (argc < 3
		|| !ParsePositiveNumber(argv[1], static_cast<Core::uint32>(-1), workerCount)
		|| !ParsePositiveNumber(argv[2], 65535, port)) {
		fprintf(stderr, "%s", usageMsg);
		return 1;
	}
	auto matchPrefix = [](const char* arg, const char* prefix, const char*& outValue) noexcept -> bool {
		const Core::uint64 len = static_cast<Core::uint64>(strlen(prefix));
		if (strncmp(arg, prefix, len) != 0)
			return false;
		outValue = arg + len;
		return true;
	};
	for (int i = 3; i < argc; ++i) {
		const char* val = nullptr;
		if (strcmp(argv[i], "--trace=stream") == 0)
			traceMode = Core::ERuntimeTraceMode::AsyncStream;
		else if (strcmp(argv[i], "--trace=flight") == 0)
			traceMode = Core::ERuntimeTraceMode::FlightRecorder;
		else if (strcmp(argv[i], "--trace=off") == 0)
			traceMode = Core::ERuntimeTraceMode::Off;
		else if (strcmp(argv[i], "--db=mysql") == 0)
			dbConfig.Enabled = true;
		else if (strcmp(argv[i], "--db=off") == 0)
			dbConfig.Enabled = false;
		else if (matchPrefix(argv[i], "--idle-timeout-ms=", val)) {
			Core::uint32 idleMs = 0;
			if (!ParseNonNegativeNumber(val, 3600000, idleMs)) {
				fprintf(stderr, "%s", usageMsg);
				return 1;
			}
			idleTimeout = Core::MILLISECONDS{ idleMs };
		}
		else if (matchPrefix(argv[i], "--db-host=", val))
			dbConfig.Host = Core::FString{ val };
		else if (matchPrefix(argv[i], "--db-port=", val)) {
			Core::uint32 dbPort = 0;
			if (!ParsePositiveNumber(val, 65535, dbPort)) {
				fprintf(stderr, "%s", usageMsg);
				return 1;
			}
			dbConfig.Port = static_cast<Core::uint16>(dbPort);
		}
		else if (matchPrefix(argv[i], "--db-user=", val))
			dbConfig.User = Core::FString{ val };
		else if (matchPrefix(argv[i], "--db-pass=", val))
			dbConfig.Password = Core::FString{ val };
		else if (matchPrefix(argv[i], "--db-name=", val))
			dbConfig.Database = Core::FString{ val };
		else if (matchPrefix(argv[i], "--db-workers=", val)) {
			Core::uint32 dbWorkers = 0;
			if (!ParsePositiveNumber(val, 16, dbWorkers)) {
				fprintf(stderr, "%s", usageMsg);
				return 1;
			}
			dbConfig.WorkerCount = dbWorkers;
		}
		else if (matchPrefix(argv[i], "--pbkdf2-iters=", val)) {
			Core::uint32 iters = 0;
			if (!ParsePositiveNumber(val, 300000, iters) || iters < 1000) {
				fprintf(stderr, "%s", usageMsg);
				return 1;
			}
			dbConfig.Pbkdf2Iterations = iters;
		}
		else {
			fprintf(stderr, "%s", usageMsg);
			return 1;
		}
	}

	Core::FRuntimeTraceRun runtimeTrace{ traceMode };
	RUNTIME_TRACE_SCOPE(ServerApp, main, 0);

	bool success = false;
	{
		Server::UServerApp serverApp;
		if (serverApp.Start(static_cast<Core::uint16>(port), idleTimeout, dbConfig)) {
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
