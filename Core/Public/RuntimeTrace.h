#pragma once

#include "CoreTypes.h"
#include "CoreMacro.h"
#ifdef _WIN32
#include "CoreDefine.h"
#define CORE_TRACE_API CORE_DLL
#else
#define CORE_TRACE_API
#endif

SPACE_H(Core)

enum class ERuntimeTraceEvent : uint32 {
    Create = 1, Destroy, Call, Submit, Callback, End
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
    FRuntimeTraceRun() noexcept;
    ~FRuntimeTraceRun();
    NO_COPY(FRuntimeTraceRun)

private:
    bool _OwnsFile{};
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

#ifndef _WIN32
// 독립 Unix ServerInput 빌드에는 Windows 파일 기록기가 없다.
inline FRuntimeTraceRun::FRuntimeTraceRun() noexcept = default;
inline FRuntimeTraceRun::~FRuntimeTraceRun() = default;
inline FRuntimeTraceID::FRuntimeTraceID() noexcept : _Value{} {}
inline FRuntimeTraceID::FRuntimeTraceID(const FRuntimeTraceID&) noexcept : FRuntimeTraceID{} {}
inline uint64 RecordRuntimeTrace(ERuntimeTraceEvent, uint64, FRuntimeTraceSite) noexcept { return 0; }
inline FRuntimeTraceScope::FRuntimeTraceScope(uint64, FRuntimeTraceSite) noexcept : _PreviousScope{}, _PreviousObject{} {}
inline FRuntimeTraceScope::FRuntimeTraceScope(uint64, FRuntimeTraceSite, uint64) noexcept : _PreviousScope{}, _PreviousObject{} {}
inline FRuntimeTraceScope::~FRuntimeTraceScope() = default;
#endif

SPACE_T
#undef CORE_TRACE_API

// 파일마다 프로젝트 기준 경로를 한 번 명시한다. 빌드 폴더와 /FC 설정이 달라도 같은 소스 줄을 가리킨다.
#define RUNTIME_TRACE_SITE(type, method) Core::FRuntimeTraceSite{ #type, #method, RUNTIME_TRACE_SOURCE, static_cast<Core::uint32>(__LINE__) }
#define RUNTIME_TRACE_SCOPE(type, method, object) \
    Core::FRuntimeTraceScope runtimeScope{ object, RUNTIME_TRACE_SITE(type, method) }
#define RUNTIME_TRACE_EVENT(kind, type, method, object) \
    Core::RecordRuntimeTrace(Core::ERuntimeTraceEvent::kind, object, RUNTIME_TRACE_SITE(type, method))
#define RUNTIME_TRACE_CALLBACK(type, method, object, submission) \
    Core::FRuntimeTraceScope runtimeCallbackScope{ object, RUNTIME_TRACE_SITE(type, method), submission }
