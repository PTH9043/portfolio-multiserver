#include "CoreDefine.h"
#include "RuntimeTrace.h"

#if defined(_WIN32) && CORE_ENABLE_RUNTIME_TRACE
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

STATIC_CONSTEXPR uint64 STREAM_BUFFER_CAPACITY{ 4096 };
STATIC_CONSTEXPR uint64 STREAM_FLUSH_THRESHOLD{ 256 };
STATIC_CONSTEXPR MILLISECONDS STREAM_FLUSH_INTERVAL{ 50 };
STATIC_CONSTEXPR uint64 FLIGHT_RING_CAPACITY{ 16384 };

SHARED_MUTEX traceLock;
SHARED_MUTEX logFileLock;
COND_VARIABLE writerWakeCV;
COND_VARIABLE writerSpaceCV;
HANDLE traceFile = INVALID_HANDLE_VALUE;
HANDLE logFile = INVALID_HANDLE_VALUE;
bool logFileOpenFailed{};
ATOMIC_BOOL recording{};
ATOMIC<ERuntimeTraceMode> activeMode{ ERuntimeTraceMode::Off };
ATOMIC_UINT64 lastObject{};
uint64 lastSequence{};
uint64 activeRunID{};
DWORD activeProcessID{};

// AsyncStream 이중 버퍼(네트워크·방 워커는 메모리 복사만 하고 디스크 WriteFile은 전용 스레드가 일괄 처리)
UQPTR<FTraceRecord[]> frontBuffer;
UQPTR<FTraceRecord[]> backBuffer;
uint64 frontCount{};
uint64 backCount{};
bool writerBusy{};
bool writerStop{};
bool flushRequested{};
THREAD writerThread;

// FlightRecorder 메모리 링버퍼(평소 디스크 I/O 0회, Error 또는 덤프 요청 시 .cvtrace로 일괄 저장)
UQPTR<FTraceRecord[]> flightRing;
uint64 flightTotalWritten{};
bool flightDumped{};

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
    const uint8* cursor = static_cast<const uint8*>(data);
    DWORD remaining = length;
    while (remaining > 0) {
        DWORD written{};
        if (!WriteFile(file, cursor, remaining, &written, nullptr))
            return false;
        if (written == 0) {
            SetLastError(ERROR_WRITE_FAULT);
            return false;
        }
        cursor += written;
        remaining -= written;
    }
    return true;
}

template<uint64 N>
void CopyName(char (&target)[N], const char* source) noexcept
{
    const uint64 length = strnlen_s(source, N - 1);
    std::memcpy(target, source, length);
    target[length] = '\0';
}

void FillRecord(FTraceRecord& event, uint64 sequence, ERuntimeTraceEvent kind, uint64 object,
    FRuntimeTraceSite site, uint64 parent, uint64 caller) noexcept
{
    FILETIME time{};
    GetSystemTimePreciseAsFileTime(&time);
    event.Sequence = sequence;
    event.Time = FileTimeValue(time);
    event.Object = object;
    event.CallerObject = caller;
    event.Parent = parent;
    event.ThreadID = GetCurrentThreadId();
    event.Line = site.Line;
    event.Kind = static_cast<uint32>(kind);
    event.Reserved = 0;
    CopyName(event.Type, site.Type);
    CopyName(event.Function, site.Function);
    CopyName(event.Source, site.Source);
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
    HANDLE _Handle{ INVALID_HANDLE_VALUE };
};

HANDLE CreateArtifactFile(uint64 runID, DWORD pid, const wchar_t* extension, DWORD& error)
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
    wchar_t name[64]{};
    swprintf_s(name, L"%llu-%lu.%ls", runID, pid, extension);
    const auto path = directory / name;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        error = GetLastError();
    return file;
}

HANDLE OpenAndWriteHeader(uint64 runID, DWORD pid, DWORD& openError) noexcept
{
    HANDLE file = INVALID_HANDLE_VALUE;
    try {
        file = CreateArtifactFile(runID, pid, L"cvtrace", openError);
    }
    catch (const std::filesystem::filesystem_error& error) {
        openError = static_cast<DWORD>(error.code().value());
    }
    catch (...) {
        openError = ERROR_NOT_ENOUGH_MEMORY;
    }
    if (file == INVALID_HANDLE_VALUE)
        return INVALID_HANDLE_VALUE;

    FTraceHeader header{};
    std::memcpy(header.Magic, "CVTRACE1", sizeof(header.Magic));
    header.Version = 1;
    header.HeaderBytes = sizeof(FTraceHeader);
    header.RecordBytes = sizeof(FTraceRecord);
    header.ProcessID = pid;
    header.Flags = 1;
    header.RunID = runID;
    if (!WriteBytes(file, &header, sizeof(header))) {
        openError = GetLastError();
        CloseHandle(file);
        return INVALID_HANDLE_VALUE;
    }
    return file;
}

void RunAsyncWriter() noexcept
{
    for (;;) {
        uint64 countToWrite{};
        HANDLE fileHandle = INVALID_HANDLE_VALUE;
        {
            std::unique_lock<SHARED_MUTEX> lock{ traceLock };
            if (frontCount == 0 && !writerStop) {
                writerWakeCV.wait_for(lock, STREAM_FLUSH_INTERVAL, [] {
                    return frontCount >= STREAM_FLUSH_THRESHOLD || flushRequested || writerStop;
                });
            }
            if (frontCount == 0 && writerStop)
                break;
            if (frontCount == 0) {
                flushRequested = false;
                continue;
            }
            std::swap(frontBuffer, backBuffer);
            countToWrite = frontCount;
            backCount = countToWrite;
            frontCount = 0;
            flushRequested = false;
            writerBusy = true;
            fileHandle = traceFile;
        }
        writerSpaceCV.notify_all();

        bool writeOk = false;
        DWORD writeError{};
        if (fileHandle != INVALID_HANDLE_VALUE && countToWrite > 0) {
            const DWORD totalBytes = static_cast<DWORD>(countToWrite * sizeof(FTraceRecord));
            writeOk = WriteBytes(fileHandle, backBuffer.get(), totalBytes);
            if (!writeOk)
                writeError = GetLastError();
        }

        {
            EXCLUSIVE_LOCK_GUARD guard{ traceLock };
            backCount = 0;
            writerBusy = false;
            if (!writeOk && traceFile != INVALID_HANDLE_VALUE) {
                CloseHandle(traceFile);
                traceFile = INVALID_HANDLE_VALUE;
                recording.store(false, std::memory_order_release);
                activeMode.store(ERuntimeTraceMode::Off, std::memory_order_release);
                ReportFileError(writeError);
            }
        }
        writerSpaceCV.notify_all();
    }
}

uint64 Publish(ERuntimeTraceEvent kind, uint64 object, FRuntimeTraceSite site, uint64 parent, uint64 caller) noexcept
{
    if (!recording.load(std::memory_order_acquire))
        return 0;

    const ERuntimeTraceMode mode = activeMode.load(std::memory_order_acquire);
    if (mode == ERuntimeTraceMode::Off)
        return 0;

    const DWORD savedError = GetLastError();
    uint64 sequence{};

    if (mode == ERuntimeTraceMode::FlightRecorder) {
        EXCLUSIVE_LOCK_GUARD guard{ traceLock };
        if (flightRing && recording.load(std::memory_order_relaxed)) {
            sequence = ++lastSequence;
            FTraceRecord& slot = flightRing[flightTotalWritten % FLIGHT_RING_CAPACITY];
            FillRecord(slot, sequence, kind, object, site, parent, caller);
            ++flightTotalWritten;
        }
        SetLastError(savedError);
        return sequence;
    }

    // AsyncStream: 락 안에서는 메모리 버퍼 기록만 수행하고 디스크 I/O는 백그라운드 스레드에 넘긴다.
    bool shouldWake = false;
    {
        std::unique_lock<SHARED_MUTEX> lock{ traceLock };
        while (frontCount >= STREAM_BUFFER_CAPACITY && traceFile != INVALID_HANDLE_VALUE && !writerStop) {
            writerWakeCV.notify_one();
            writerSpaceCV.wait(lock);
        }
        if (traceFile != INVALID_HANDLE_VALUE && !writerStop && frontBuffer) {
            sequence = ++lastSequence;
            FillRecord(frontBuffer[frontCount], sequence, kind, object, site, parent, caller);
            ++frontCount;
            shouldWake = (frontCount >= STREAM_FLUSH_THRESHOLD);
        }
    }
    if (shouldWake)
        writerWakeCV.notify_one();

    SetLastError(savedError);
    return sequence;
}
}

FRuntimeTraceRun::FRuntimeTraceRun(ERuntimeTraceMode mode) noexcept
{
    const DWORD pid = GetCurrentProcessId();
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        ReportFileError(GetLastError());
        return;
    }
    const uint64 runID = FileTimeValue(created);

    {
        EXCLUSIVE_LOCK_GUARD logGuard{ logFileLock };
        activeRunID = runID;
        activeProcessID = pid;
        logFileOpenFailed = false;
    }
    _OwnsSession = true;

    if (mode == ERuntimeTraceMode::Off)
        return;

    if (mode == ERuntimeTraceMode::FlightRecorder) {
        UQPTR<FTraceRecord[]> ring;
        try {
            ring = UQPTR<FTraceRecord[]>(new FTraceRecord[FLIGHT_RING_CAPACITY]{});
        }
        catch (...) {
            ReportFileError(ERROR_NOT_ENOUGH_MEMORY);
            return;
        }
        EXCLUSIVE_LOCK_GUARD guard{ traceLock };
        flightRing = std::move(ring);
        flightTotalWritten = 0;
        flightDumped = false;
        lastSequence = 0;
        activeRunID = runID;
        activeProcessID = pid;
        _OwnsSession = true;
        activeMode.store(ERuntimeTraceMode::FlightRecorder, std::memory_order_release);
        recording.store(true, std::memory_order_release);
        return;
    }

    // AsyncStream 모드: 헤더를 먼저 쓰고 이중 버퍼 + 백그라운드 기록 스레드를 기동한다.
    DWORD openError{};
    HANDLE file = OpenAndWriteHeader(runID, pid, openError);
    if (file == INVALID_HANDLE_VALUE) {
        ReportFileError(openError);
        return;
    }

    UQPTR<FTraceRecord[]> bufA;
    UQPTR<FTraceRecord[]> bufB;
    try {
        bufA = UQPTR<FTraceRecord[]>(new FTraceRecord[STREAM_BUFFER_CAPACITY]{});
        bufB = UQPTR<FTraceRecord[]>(new FTraceRecord[STREAM_BUFFER_CAPACITY]{});
    }
    catch (...) {
        CloseHandle(file);
        ReportFileError(ERROR_NOT_ENOUGH_MEMORY);
        return;
    }

    {
        EXCLUSIVE_LOCK_GUARD guard{ traceLock };
        traceFile = file;
        frontBuffer = std::move(bufA);
        backBuffer = std::move(bufB);
        frontCount = 0;
        backCount = 0;
        writerBusy = false;
        writerStop = false;
        flushRequested = false;
        lastSequence = 0;
        activeRunID = runID;
        activeProcessID = pid;
        _OwnsSession = true;
        activeMode.store(ERuntimeTraceMode::AsyncStream, std::memory_order_release);
        recording.store(true, std::memory_order_release);
    }

    try {
        writerThread = THREAD{ RunAsyncWriter };
    }
    catch (...) {
        EXCLUSIVE_LOCK_GUARD guard{ traceLock };
        recording.store(false, std::memory_order_release);
        activeMode.store(ERuntimeTraceMode::Off, std::memory_order_release);
        CloseHandle(traceFile);
        traceFile = INVALID_HANDLE_VALUE;
        frontBuffer.reset();
        backBuffer.reset();
        _OwnsSession = false;
        ReportFileError(ERROR_NOT_ENOUGH_MEMORY);
    }
}

FRuntimeTraceRun::~FRuntimeTraceRun()
{
    if (!_OwnsSession)
        return;

    {
        EXCLUSIVE_LOCK_GUARD logGuard{ logFileLock };
        if (logFile != INVALID_HANDLE_VALUE) {
            CloseHandle(logFile);
            logFile = INVALID_HANDLE_VALUE;
        }
        logFileOpenFailed = false;
    }

    const ERuntimeTraceMode mode = activeMode.load(std::memory_order_acquire);
    if (mode == ERuntimeTraceMode::Off)
        return;
    if (mode == ERuntimeTraceMode::FlightRecorder) {
        EXCLUSIVE_LOCK_GUARD guard{ traceLock };
        recording.store(false, std::memory_order_release);
        activeMode.store(ERuntimeTraceMode::Off, std::memory_order_release);
        flightRing.reset();
        flightTotalWritten = 0;
        flightDumped = false;
        return;
    }

    {
        std::unique_lock<SHARED_MUTEX> lock{ traceLock };
        recording.store(false, std::memory_order_release);
        activeMode.store(ERuntimeTraceMode::Off, std::memory_order_release);
        while (frontCount >= STREAM_BUFFER_CAPACITY && traceFile != INVALID_HANDLE_VALUE && !writerStop) {
            writerWakeCV.notify_one();
            writerSpaceCV.wait(lock);
        }
        if (traceFile != INVALID_HANDLE_VALUE && frontBuffer) {
            const uint64 endSeq = ++lastSequence;
            FillRecord(frontBuffer[frontCount], endSeq, ERuntimeTraceEvent::End, 0,
                { "ServerApp", "End", "Core/Private/RuntimeTrace.cpp", static_cast<uint32>(__LINE__) }, 0, 0);
            ++frontCount;
        }
        writerStop = true;
        flushRequested = true;
    }
    writerWakeCV.notify_all();
    writerSpaceCV.notify_all();

    if (writerThread.joinable())
        writerThread.join();

    {
        EXCLUSIVE_LOCK_GUARD guard{ traceLock };
        if (traceFile != INVALID_HANDLE_VALUE) {
            CloseHandle(traceFile);
            traceFile = INVALID_HANDLE_VALUE;
        }
        frontBuffer.reset();
        backBuffer.reset();
        frontCount = 0;
        backCount = 0;
        writerStop = false;
    }
}

FRuntimeTraceID::FRuntimeTraceID() noexcept : _Value{ lastObject.fetch_add(1, std::memory_order_relaxed) + 1 }
{}

FRuntimeTraceID::FRuntimeTraceID(const FRuntimeTraceID&) noexcept : FRuntimeTraceID{}
{}

uint64 RecordRuntimeTrace(ERuntimeTraceEvent kind, uint64 object, FRuntimeTraceSite site) noexcept
{
    return Publish(kind, object, site, currentScope, currentObject);
}

bool TriggerRuntimeTraceDump() noexcept
{
    const ERuntimeTraceMode mode = activeMode.load(std::memory_order_acquire);
    if (mode == ERuntimeTraceMode::Off)
        return false;

    if (mode == ERuntimeTraceMode::AsyncStream) {
        {
            EXCLUSIVE_LOCK_GUARD guard{ traceLock };
            if (traceFile == INVALID_HANDLE_VALUE)
                return false;
            flushRequested = true;
        }
        writerWakeCV.notify_one();
        return true;
    }

    // FlightRecorder 모드: 링버퍼 스냅샷을 1..N 연속 순번으로 재매핑해 ChangeViewer가 그대로 읽는 .cvtrace를 생성한다.
    UQPTR<FTraceRecord[]> snapshot;
    uint64 count{};
    uint64 runID{};
    DWORD pid{};
    {
        EXCLUSIVE_LOCK_GUARD guard{ traceLock };
        if (!flightRing || flightDumped || flightTotalWritten == 0)
            return false;

        count = (flightTotalWritten < FLIGHT_RING_CAPACITY) ? flightTotalWritten : FLIGHT_RING_CAPACITY;
        try {
            snapshot = UQPTR<FTraceRecord[]>(new FTraceRecord[count + 1]);
        }
        catch (...) {
            return false;
        }

        const uint64 startPos = (flightTotalWritten > FLIGHT_RING_CAPACITY)
            ? (flightTotalWritten - FLIGHT_RING_CAPACITY) : 0;
        const uint64 firstOldSeq = flightRing[startPos % FLIGHT_RING_CAPACITY].Sequence;

        for (uint64 i = 0; i < count; ++i) {
            FTraceRecord rec = flightRing[(startPos + i) % FLIGHT_RING_CAPACITY];
            rec.Sequence = i + 1;
            if (rec.Parent >= firstOldSeq && rec.Parent < firstOldSeq + i)
                rec.Parent = (rec.Parent - firstOldSeq) + 1;
            else
                rec.Parent = 0;
            snapshot[i] = rec;
        }
        FillRecord(snapshot[count], count + 1, ERuntimeTraceEvent::End, 0,
            { "ServerApp", "End", "Core/Private/RuntimeTrace.cpp", static_cast<uint32>(__LINE__) }, 0, 0);
        flightDumped = true;
        runID = activeRunID;
        pid = activeProcessID;
    }

    DWORD openError{};
    HANDLE file = OpenAndWriteHeader(runID, pid, openError);
    if (file == INVALID_HANDLE_VALUE) {
        ReportFileError(openError);
        return false;
    }
    const DWORD totalBytes = static_cast<DWORD>((count + 1) * sizeof(FTraceRecord));
    const bool ok = WriteBytes(file, snapshot.get(), totalBytes);
    if (!ok)
        ReportFileError(GetLastError());
    CloseHandle(file);
    return ok;
}

ERuntimeTraceMode GetRuntimeTraceMode() noexcept
{
    return activeMode.load(std::memory_order_acquire);
}

uint64 GetCurrentRuntimeTraceSeq() noexcept
{
    return currentScope;
}

uint64 GetCurrentRuntimeTraceObject() noexcept
{
    return currentObject;
}

uint32 GetCurrentRuntimeTraceThreadID() noexcept
{
    return static_cast<uint32>(GetCurrentThreadId());
}

bool AppendRuntimeLogBatch(STRINGVIEW bytes) noexcept
{
    if (bytes.empty())
        return true;

    const DWORD savedError = GetLastError();
    bool ok = false;
    {
        EXCLUSIVE_LOCK_GUARD guard{ logFileLock };
        if (logFile == INVALID_HANDLE_VALUE) {
            if (!logFileOpenFailed && activeRunID != 0 && activeProcessID != 0) {
                DWORD openError{};
                try {
                    logFile = CreateArtifactFile(activeRunID, activeProcessID, L"cvlog", openError);
                }
                catch (...) {
                    logFile = INVALID_HANDLE_VALUE;
                }
                if (logFile == INVALID_HANDLE_VALUE)
                    logFileOpenFailed = true;
            }
        }

        if (logFile != INVALID_HANDLE_VALUE) {
            const uint8* cursor = reinterpret_cast<const uint8*>(bytes.data());
            uint64 remaining = static_cast<uint64>(bytes.size());
            ok = true;
            while (remaining > 0) {
                const DWORD chunk = static_cast<DWORD>(remaining > 0x40000000ULL ? 0x40000000ULL : remaining);
                if (!WriteBytes(logFile, cursor, chunk)) {
                    CloseHandle(logFile);
                    logFile = INVALID_HANDLE_VALUE;
                    logFileOpenFailed = true;
                    ok = false;
                    break;
                }
                cursor += chunk;
                remaining -= chunk;
            }
        }
    }
    SetLastError(savedError);
    return ok;
}

FRuntimeTraceScope::FRuntimeTraceScope(uint64 object, FRuntimeTraceSite site) noexcept
    : _PreviousScope{ currentScope }, _PreviousObject{ currentObject }
{
    currentScope = Publish(ERuntimeTraceEvent::Call, object, site, currentScope, currentObject);
    currentObject = currentScope ? object : 0;
}

FRuntimeTraceScope::FRuntimeTraceScope(uint64 object, FRuntimeTraceSite site, uint64 submission) noexcept
    : _PreviousScope{ currentScope }, _PreviousObject{ currentObject }
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
