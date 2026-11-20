#include "CoreDefine.h"
#include "UListener.h"
#include "UCoreInstance.h"
#include "ULogger.h"

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/UListener.cpp"

SPACE_H(Core)

UListener::UListener(SHPTR<UCoreInstance> coreInstance, IOCONTEXT& context)
    : UObject{ coreInstance }, _Acceptor{ context }
{}

UListener::~UListener() = default;

bool UListener::Start(const TCP_ENDPOINT& endpoint,
    FOnFailure onFailure, BOOST_ERROR_CODE& error)
{
    RUNTIME_TRACE_SCOPE(UListener, Start, GetRuntimeTraceID());
    error.clear();
    if (!onFailure || ThisWeak<UListener>().expired()) {
        error = ASIO::error::invalid_argument;
        return false;
    }
    if (_Started || _Running || _Acceptor.is_open()) {
        error = ASIO::error::already_started;
        return false;
    }
    auto handlers = MakeShared<FHandlers>(FHandlers{ std::move(onFailure) });

    _Acceptor.open(endpoint.protocol(), error);
    if (error) return false;
    _Acceptor.bind(endpoint, error);
    if (!error) _Acceptor.listen(ASIO::socket_base::max_listen_connections, error);
    if (error) {
        BOOST_ERROR_CODE closeError;
        _Acceptor.close(closeError);
        LogCleanupError("UListener::Start cleanup close failed", closeError);
        return false;
    }

    _Handlers = std::move(handlers);
    _Running = true;
    try {
        BeginAccept();
        _Started = true;
    }
    catch (...) {
        // 수락 등록에 실패했으므로 완료 핸들러가 없고, 잠금 없이 정리해도 된다.
        _Running = false;
        _Handlers.reset();
        BOOST_ERROR_CODE closeError;
        _Acceptor.close(closeError);
        LogCleanupError("UListener::Start cleanup close failed", closeError);
        throw;
    }
    return true;
}

void UListener::Stop(BOOST_ERROR_CODE& error) noexcept
{
    RUNTIME_TRACE_SCOPE(UListener, Stop, GetRuntimeTraceID());
    error.clear();
    // 핸들러는 잠금 밖에서 해제되도록 잠금 범위 밖의 변수로 옮긴다.
    SHPTR<FHandlers> retired;
    EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
    _Running = false;
    retired.swap(_Handlers);
    if (_Acceptor.is_open()) _Acceptor.close(error);
}

void UListener::BeginAccept()
{
    RUNTIME_TRACE_SCOPE(UListener, BeginAccept, GetRuntimeTraceID());
    WKPTR<UListener> weak = ThisWeak<UListener>();
    const auto submission = RUNTIME_TRACE_EVENT(Submit, UListener, BeginAccept, GetRuntimeTraceID());
    _Acceptor.async_accept([weak, submission](const BOOST_ERROR_CODE& error,
        TCPSOCKET socket) {
        if (auto listener = weak.lock()) {
            RUNTIME_TRACE_CALLBACK(UListener, BeginAccept, listener->GetRuntimeTraceID(), submission);
            listener->OnAccept(error, std::move(socket));
        }
    });
}

void UListener::OnAccept(const BOOST_ERROR_CODE& error,
    TCPSOCKET socket)
{
    RUNTIME_TRACE_SCOPE(UListener, OnAccept, GetRuntimeTraceID());
    SHPTR<FHandlers> handlers;
    BOOST_ERROR_CODE closeError; // 잠금 안에서 닫고, 기록은 잠금 밖에서 한다.
    {
        EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
        if (!_Running)
            return;
        handlers = _Handlers;
        if (error) {
            _Running = false;
            _Handlers.reset(); // handlers가 참조를 보유하므로 해제는 잠금 밖에서 일어난다.
            _Acceptor.close(closeError);
        }
    }

    if (error) {
        LogCleanupError("UListener::OnAccept cleanup close failed", closeError);
        handlers->OnFailure(error);
        return;
    }

    try {
        HandleAcceptedSocket(std::move(socket));
    }
    catch (...) {
        BOOST_ERROR_CODE stopError;
        Stop(stopError);
        LogCleanupError("UListener::OnAccept cleanup stop failed", stopError);
        throw;
    }

    handlers.reset();
    try {
        EXCLUSIVE_LOCK_GUARD lock{ _Mutex };
        if (_Running)
            BeginAccept();
    }
    catch (...) {
        BOOST_ERROR_CODE stopError;
        Stop(stopError);
        LogCleanupError("UListener::OnAccept cleanup stop failed", stopError);
        throw;
    }
}

void UListener::LogCleanupError(const STRINGVIEW where, const BOOST_ERROR_CODE& error) noexcept
{
    RUNTIME_TRACE_SCOPE(UListener, LogCleanupError, GetRuntimeTraceID());
    if (!error)
        return;
    const auto coreInstance = GetCoreInstance();
    const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
    if (logger)
        logger->TryLogError(where, error);
}

SPACE_T
