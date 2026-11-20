#pragma once

#include "CoreTypes.h"
#include "CoreMacro.h"
#ifdef _WIN32
#include "CoreDefine.h"
#define CORE_TRACE_API CORE_DLL
#else
#define CORE_TRACE_API
#endif

// 0으로 정의해 빌드하면 모든 RUNTIME_TRACE_* 매크로가 컴파일 시 완전히 제거된다(기계어 0바이트).
#ifndef CORE_ENABLE_RUNTIME_TRACE
#define CORE_ENABLE_RUNTIME_TRACE 1
#endif

SPACE_H(Core)

enum class ERuntimeTraceEvent : uint32 {
    Create = 1, Destroy, Call, Submit, Callback, End
};

// Off: 기록 비활성화 / AsyncStream: 비동기 이중 버퍼 일괄 기록(ChangeViewer 실시간 연동)
// FlightRecorder: 평소 디스크 I/O 0회 메모리 링버퍼 보관, ULogger Error 또는 수동 트리거 시 .cvtrace 덤프
enum class ERuntimeTraceMode : uint8 {
    Off = 0,
    AsyncStream = 1,
    FlightRecorder = 2
};

struct FRuntimeTraceSite {
    const char* Type;
    const char* Function;
    const char* Source;
    uint32 Line;
};

// 프로세스에서 한 번, main에서 서버 객체보다 먼저 만들고 마지막에 파괴한다.
class CORE_TRACE_API FRuntimeTraceRun final {
public:
    explicit FRuntimeTraceRun(ERuntimeTraceMode mode = ERuntimeTraceMode::AsyncStream) noexcept;
    ~FRuntimeTraceRun();
    NO_COPY(FRuntimeTraceRun)

private:
    bool _OwnsSession{};
};

// 주소를 다시 사용하거나 객체를 복사해도 새 수명은 새 번호를 받는다. 대입은 대상의 번호를 유지한다.
class CORE_TRACE_API FRuntimeTraceID final {
public:
    FRuntimeTraceID() noexcept;
    FRuntimeTraceID(const FRuntimeTraceID&) noexcept;
    FRuntimeTraceID& operator=(const FRuntimeTraceID&) noexcept { return *this; }
    uint64 Get() const noexcept { return _Value; }

private:
    uint64 _Value;
};

// Submit은 게시를 시도한 위치다. Callback에서 전달받은 번호가 실제 실행을 증명한다.
CORE_TRACE_API uint64 RecordRuntimeTrace(ERuntimeTraceEvent kind, uint64 object, FRuntimeTraceSite site) noexcept;
// FlightRecorder 모드에서는 링버퍼를 .cvtrace로 덤프하고, AsyncStream 모드에서는 대기 배치를 즉시 플러시한다.
CORE_TRACE_API bool TriggerRuntimeTraceDump() noexcept;
CORE_TRACE_API ERuntimeTraceMode GetRuntimeTraceMode() noexcept;
// 현재 스레드의 활성 계측 순번·객체 ID·OS 스레드 ID를 돌려준다.
CORE_TRACE_API uint64 GetCurrentRuntimeTraceSeq() noexcept;
CORE_TRACE_API uint64 GetCurrentRuntimeTraceObject() noexcept;
CORE_TRACE_API uint32 GetCurrentRuntimeTraceThreadID() noexcept;
// ULogger 배치 로그(JSONL)를 현재 실행의 ChangeViewerTrace/<runId>-<pid>.cvlog에 기록한다.
CORE_TRACE_API bool AppendRuntimeLogBatch(STRINGVIEW bytes) noexcept;

class CORE_TRACE_API FRuntimeTraceScope final {
public:
    FRuntimeTraceScope(uint64 object, FRuntimeTraceSite site) noexcept;
    FRuntimeTraceScope(uint64 object, FRuntimeTraceSite site, uint64 submission) noexcept;
    ~FRuntimeTraceScope();
    NO_COPY(FRuntimeTraceScope)

private:
    uint64 _PreviousScope;
    uint64 _PreviousObject;
};

#if !defined(_WIN32) || !CORE_ENABLE_RUNTIME_TRACE
inline FRuntimeTraceRun::FRuntimeTraceRun(ERuntimeTraceMode) noexcept {}
inline FRuntimeTraceRun::~FRuntimeTraceRun() = default;
inline FRuntimeTraceID::FRuntimeTraceID() noexcept : _Value{} {}
inline FRuntimeTraceID::FRuntimeTraceID(const FRuntimeTraceID&) noexcept : FRuntimeTraceID{} {}
inline uint64 RecordRuntimeTrace(ERuntimeTraceEvent, uint64, FRuntimeTraceSite) noexcept { return 0; }
inline bool TriggerRuntimeTraceDump() noexcept { return false; }
inline ERuntimeTraceMode GetRuntimeTraceMode() noexcept { return ERuntimeTraceMode::Off; }
inline uint64 GetCurrentRuntimeTraceSeq() noexcept { return 0; }
inline uint64 GetCurrentRuntimeTraceObject() noexcept { return 0; }
inline uint32 GetCurrentRuntimeTraceThreadID() noexcept { return 0; }
inline bool AppendRuntimeLogBatch(STRINGVIEW) noexcept { return false; }
inline FRuntimeTraceScope::FRuntimeTraceScope(uint64, FRuntimeTraceSite) noexcept : _PreviousScope{}, _PreviousObject{} {}
inline FRuntimeTraceScope::FRuntimeTraceScope(uint64, FRuntimeTraceSite, uint64) noexcept : _PreviousScope{}, _PreviousObject{} {}
inline FRuntimeTraceScope::~FRuntimeTraceScope() = default;
#endif

SPACE_T
#undef CORE_TRACE_API

#if CORE_ENABLE_RUNTIME_TRACE
// 파일마다 프로젝트 기준 경로를 한 번 명시한다. 빌드 폴더와 /FC 설정이 달라도 같은 소스 줄을 가리킨다.
#define RUNTIME_TRACE_SITE(type, method) Core::FRuntimeTraceSite{ #type, #method, RUNTIME_TRACE_SOURCE, static_cast<Core::uint32>(__LINE__) }
#define RUNTIME_TRACE_SCOPE(type, method, object) \
    Core::FRuntimeTraceScope runtimeScope{ object, RUNTIME_TRACE_SITE(type, method) }
#define RUNTIME_TRACE_EVENT(kind, type, method, object) \
    Core::RecordRuntimeTrace(Core::ERuntimeTraceEvent::kind, object, RUNTIME_TRACE_SITE(type, method))
#define RUNTIME_TRACE_CALLBACK(type, method, object, submission) \
    Core::FRuntimeTraceScope runtimeCallbackScope{ object, RUNTIME_TRACE_SITE(type, method), submission }
#else
#define RUNTIME_TRACE_SITE(type, method) Core::FRuntimeTraceSite{ "", "", "", 0 }
#define RUNTIME_TRACE_SCOPE(type, method, object) ((void)0)
#define RUNTIME_TRACE_EVENT(kind, type, method, object) (static_cast<Core::uint64>(0))
#define RUNTIME_TRACE_CALLBACK(type, method, object, submission) ((void)0)
#endif
