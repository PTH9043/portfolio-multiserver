#include "ServerApp.h"

#include <crtdbg.h>
#include <stdio.h>

namespace {
bool ParseWorkerCount(const char* text, Core::uint32& count)
{
	if (nullptr == text)
		return false;
	if ('\0' == *text)
		return false;

	const Core::uint32 maxCount = static_cast<Core::uint32>(-1);
	Core::uint32 value = 0;
	for (; '\0' != *text; ++text) {
		if (*text < '0' || *text > '9')
			return false;
		const Core::uint32 digit = static_cast<Core::uint32>(*text - '0');
		if (value > (maxCount - digit) / 10)
			return false;
		value = value * 10 + digit;
	}
	if (value == 0)
		return false;

	count = value;
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

	Core::uint32 workerCount = 0;
	if (argc != 2 || !ParseWorkerCount(argv[1], workerCount)) {
		fprintf(stderr, "Usage: ServerApp <network-worker-count> (positive integer)\n");
		return 1;
	}

	bool success = false;
	{
		Server::UServerApp serverApp;
		if (serverApp.Start()) {
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
