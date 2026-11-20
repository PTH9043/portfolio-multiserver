#include "CoreDefine.h"
#include "RuntimeTrace.h"

#ifdef _WIN32
#include <Windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <filesystem>

SPACE_H(Core)
namespace {
struct FTraceHeader {
    char Magic[8];
    uint32 Version;
    uint32 HeaderBytes;
    uint32 RecordBytes;
    uint32 Reserved;
    uint32 ProcessID;
    uint32 Flags;
    uint64 RunID;
    uint64 Padding[3];
};

struct FTraceRecord {
    uint64 Sequence;
    uint64 Time;
    uint64 Object;
    uint64 CallerObject;
    uint64 Parent;
    uint32 ThreadID;
    uint32 Line;
    uint32 Kind;
    uint32 Reserved;
    char Type[64];
    char Function[64];
    char Source[72];
};

static_assert(sizeof(FTraceHeader) == 64 && offsetof(FTraceHeader, RunID) == 32);
static_assert(sizeof(FTraceRecord) == 256 && offsetof(FTraceRecord, Source) == 184);

SHARED_MUTEX traceLock;
HANDLE traceFile = INVALID_HANDLE_VALUE;
ATOMIC_BOOL recording{};
ATOMIC_UINT64 lastObject{};
uint64 lastSequence{};
thread_local uint64 currentScope{};
thread_local uint64 currentObject{};

uint64 FileTimeValue(FILETIME time) noexcept
{
    return (static_cast<uint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

void ReportFileError(DWORD error) noexcept
{
    // Logger가 시작하기 전이나 종료한 뒤에도 관찰 오류는 한 번만 알리고 서버는 계속 실행한다.
    std::fprintf(stderr, "ChangeViewer: runtime trace stopped (Windows error %lu).\n", error);
}

bool WriteBytes(HANDLE file, const void* data, DWORD length) noexcept
{
    DWORD written{};
    if (!WriteFile(file, data, length, &written, nullptr))
        return false;
    if (written == length)
        return true;
    SetLastError(ERROR_WRITE_FAULT);
    return false;
}

template<uint64 N>
void CopyName(char (&target)[N], const char* source) noexcept
{
    const uint64 length = strnlen_s(source, N - 1);
    std::memcpy(target, source, length);
    target[length] = '\0';
}

// seq를 정한 스레드가 바로 그 레코드를 쓴다. 서버에 과거 이벤트를 모아 두지 않는다.
uint64 WriteRecord(ERuntimeTraceEvent kind, uint64 object, FRuntimeTraceSite site, uint64 parent, uint64 caller) noexcept
{
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    FTraceRecord event{};
    event.Sequence = lastSequence + 1;
    event.Time = FileTimeValue(time);
    event.Object = object;
    event.CallerObject = caller;
    event.Parent = parent;
    event.ThreadID = GetCurrentThreadId();
    event.Line = site.Line;
    event.Kind = static_cast<uint32>(kind);
    CopyName(event.Type, site.Type);
    CopyName(event.Function, site.Function);
    CopyName(event.Source, site.Source);
    if (!WriteBytes(traceFile, &event, sizeof(event))) {
        const DWORD error = GetLastError();
        CloseHandle(traceFile);
        traceFile = INVALID_HANDLE_VALUE;
        recording.store(false, std::memory_order_release);
        ReportFileError(error);
        return 0;
    }
    return ++lastSequence;
}

uint64 Publish(ERuntimeTraceEvent kind, uint64 object, FRuntimeTraceSite site, uint64 parent, uint64 caller) noexcept
{
    if (!recording.load(std::memory_order_acquire))
        return 0;
    const DWORD savedError = GetLastError();
    uint64 sequence{};
    {
        EXCLUSIVE_LOCK_GUARD guard{traceLock};
        if (traceFile != INVALID_HANDLE_VALUE)
            sequence = WriteRecord(kind, object, site, parent, caller);
    }
    SetLastError(savedError);
    return sequence;
}

// 파일을 열 때까지 상위 디렉터리를 유지한다. 디렉터리 정션·심볼릭 링크는 기록 대상으로 따라가지 않는다.
class FTraceDirectory final {
public:
    ~FTraceDirectory() {
        if (_Handle != INVALID_HANDLE_VALUE)
            CloseHandle(_Handle);
    }
    NO_COPY(FTraceDirectory)
    FTraceDirectory() = default;

    bool Open(const std::filesystem::path& path, DWORD& error) noexcept {
        _Handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (_Handle == INVALID_HANDLE_VALUE) {
            error = GetLastError();
            return false;
        }
        FILE_ATTRIBUTE_TAG_INFO info{};
        if (!GetFileInformationByHandleEx(_Handle, FileAttributeTagInfo, &info, sizeof(info))) {
            error = GetLastError();
            return false;
        }
        if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
            error = ERROR_CANT_ACCESS_FILE;
            return false;
        }
        return true;
    }

private:
    HANDLE _Handle{INVALID_HANDLE_VALUE};
};

HANDLE CreateTraceFile(uint64 runID, DWORD pid, DWORD& error)
{
    wchar_t moduleName[32768]{};
    const DWORD length = GetModuleFileNameW(nullptr, moduleName, static_cast<DWORD>(std::size(moduleName)));
    if (!length || length == std::size(moduleName)) {
        error = length ? ERROR_INSUFFICIENT_BUFFER : GetLastError();
        return INVALID_HANDLE_VALUE;
    }
    const std::filesystem::path binaryDirectory = std::filesystem::path(moduleName).parent_path();
    // x64 빌드는 Bin/debug 또는 Bin/release, Win32 빌드는 Bin에 실행 파일을 둔다.
    auto bin = binaryDirectory;
    while (!bin.empty() && bin != bin.root_path()) {
        if (_wcsicmp(bin.filename().c_str(), L"Bin") == 0 &&
            _wcsicmp(bin.parent_path().filename().c_str(), L"Server") == 0)
            break;
        bin = bin.parent_path();
    }
    if (bin.empty() || bin == bin.root_path()) {
        error = ERROR_PATH_NOT_FOUND;
        return INVALID_HANDLE_VALUE;
    }
    // 프로젝트 자체가 다른 이름으로 열려 있어도 승인된 실제 루트 아래 Server/Bin만 쓴다.
    const auto project = std::filesystem::canonical(bin.parent_path().parent_path());
    const auto server = project / L"Server";
    bin = server / L"Bin";
    FTraceDirectory serverDirectory;
    FTraceDirectory binDirectory;
    if (!serverDirectory.Open(server, error) || !binDirectory.Open(bin, error))
        return INVALID_HANDLE_VALUE;
    const auto directory = bin / L"ChangeViewerTrace";
    if (!CreateDirectoryW(directory.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
        error = GetLastError();
        return INVALID_HANDLE_VALUE;
    }
    FTraceDirectory traceDirectory;
    if (!traceDirectory.Open(directory, error))
        return INVALID_HANDLE_VALUE;
    wchar_t name[56]{};
    swprintf_s(name, L"%llu-%lu.cvtrace", runID, pid);
    const auto path = directory / name;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        error = GetLastError();
    return file;
}
}

FRuntimeTraceRun::FRuntimeTraceRun() noexcept
{
    const DWORD pid = GetCurrentProcessId();
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ReportFileError(GetLastError());
        return;
    }
    const uint64 runID = FileTimeValue(created);
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD openError{};
    try {
        file = CreateTraceFile(runID, pid, openError);
    }
    catch (const std::filesystem::filesystem_error& error) {
        openError = static_cast<DWORD>(error.code().value());
    }
    catch (...) {
        openError = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (file == INVALID_HANDLE_VALUE) {
        ReportFileError(openError);
        return;
    }
    FTraceHeader header{};
    std::memcpy(header.Magic, "CVTRACE1", sizeof(header.Magic));
    header.Version = 1;
    header.HeaderBytes = sizeof(FTraceHeader);
    header.RecordBytes = sizeof(FTraceRecord);
    header.ProcessID = pid;
    header.Flags = 1;
    header.RunID = runID;
    if (!WriteBytes(file, &header, sizeof(header))) {
        const DWORD error = GetLastError();
        CloseHandle(file);
        ReportFileError(error);
        return;
    }
    EXCLUSIVE_LOCK_GUARD guard{traceLock};
    traceFile = file;
    lastSequence = 0;
    _OwnsFile = true;
    recording.store(true, std::memory_order_release);
}

FRuntimeTraceRun::~FRuntimeTraceRun()
{
    if (!_OwnsFile)
        return;
    EXCLUSIVE_LOCK_GUARD guard{traceLock};
    if (traceFile != INVALID_HANDLE_VALUE) {
        WriteRecord(ERuntimeTraceEvent::End, 0, {"ServerApp", "End", "Core/Private/RuntimeTrace.cpp", static_cast<Core::uint32>(__LINE__)}, 0, 0);
        if (traceFile != INVALID_HANDLE_VALUE) {
            CloseHandle(traceFile);
            traceFile = INVALID_HANDLE_VALUE;
        }
    }
    recording.store(false, std::memory_order_release);
}

FRuntimeTraceID::FRuntimeTraceID() noexcept : _Value{lastObject.fetch_add(1, std::memory_order_relaxed) + 1}
{}

FRuntimeTraceID::FRuntimeTraceID(const FRuntimeTraceID&) noexcept : FRuntimeTraceID{}
{}

uint64 RecordRuntimeTrace(ERuntimeTraceEvent kind, uint64 object, FRuntimeTraceSite site) noexcept
{
    return Publish(kind, object, site, currentScope, currentObject);
}

FRuntimeTraceScope::FRuntimeTraceScope(uint64 object, FRuntimeTraceSite site) noexcept
    : _PreviousScope{currentScope}, _PreviousObject{currentObject}
{
    currentScope = Publish(ERuntimeTraceEvent::Call, object, site, currentScope, currentObject);
    currentObject = currentScope ? object : 0;
}

FRuntimeTraceScope::FRuntimeTraceScope(uint64 object, FRuntimeTraceSite site, uint64 submission) noexcept
    : _PreviousScope{currentScope}, _PreviousObject{currentObject}
{
    currentScope = Publish(ERuntimeTraceEvent::Callback, object, site, submission, 0);
    currentObject = currentScope ? object : 0;
}

FRuntimeTraceScope::~FRuntimeTraceScope()
{
    currentScope = _PreviousScope;
    currentObject = _PreviousObject;
}
SPACE_T
#endif
