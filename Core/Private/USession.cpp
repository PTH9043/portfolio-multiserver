#include "CoreDefine.h"
#include "USession.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <exception>
#include <utility>

#include "RuntimeTrace.h"
#define RUNTIME_TRACE_SOURCE "Core/Private/USession.cpp"

SPACE_H(Core)

USession::USession(SHPTR<UCoreInstance> coreInstance, TCPSOCKET socket)
    : UObject{ std::move(coreInstance) },
      _Socket{ std::move(socket) },
      _Strand{ ASIO::make_strand(_Socket.get_executor()) }
{
}

USession::~USession() = default;

SHPTR<const BUFFER> USession::MakePacketBuffer(const void* data, uint64 packetSize, uint16 packetType)
{
    if (packetSize > PACKET_MAX_BODY_SIZE || (packetSize != 0 && data == nullptr))
        return {};

    FPacketHeader header{};
    header.type = packetType;
    header.size = static_cast<uint16>(packetSize);

    auto framedBuffer = MakeShared<BUFFER>(PACKET_HEADER_SIZE + packetSize);
    std::memcpy(framedBuffer->data(), &header, PACKET_HEADER_SIZE);
    if (packetSize != 0)
        std::memcpy(framedBuffer->data() + PACKET_HEADER_SIZE, data, packetSize);
    return framedBuffer;
}

uint64 USession::GetHash() const noexcept
{
    return static_cast<uint64>(std::hash<const USession*>{}(this));
}

bool USession::IsEqual(const USession& other) const noexcept
{
    return this == &other;
}

void USession::Receive()
{
    RUNTIME_TRACE_SCOPE(USession, Receive, GetRuntimeTraceID());
    // 수신 시작도 _Strand에서 해서 다른 스레드의 Close가 같은 소켓을 동시에 사용하지 않게 한다.
    // CombinePacket은 이미 _Strand에서 호출하므로 그 자리에서 바로 실행된다.
    const auto dispatchSequence = RUNTIME_TRACE_EVENT(Submit, USession, Receive, GetRuntimeTraceID());
    ASIO::dispatch(_Strand, [session = ThisShared<USession>(), dispatchSequence] {
        RUNTIME_TRACE_CALLBACK(USession, Receive, session->GetRuntimeTraceID(), dispatchSequence);
        // 먼저 Close됐으면 수신을 시작하지 않는다.
        if (!session->_Socket.is_open())
            return;

        // 완료 처리를 _Strand에서 실행해 소켓·수신 버퍼·ProcessingData 접근을 직렬화한다.
        // 완료 처리가 Session을 붙잡아 수신이 걸려 있는 동안 Session이 소멸하지 않게 한다.
        const auto readSequence = RUNTIME_TRACE_EVENT(Submit, USession, Receive, session->GetRuntimeTraceID());
        session->_Socket.async_receive(
            ASIO::buffer(session->_ReceiveBuffer.data() + session->_ReceivedBytes,
                session->_ReceiveBuffer.size() - session->_ReceivedBytes),
            ASIO::bind_executor(session->_Strand, [session, readSequence](const BOOST_ERROR_CODE& error, uint64 receivedBytes) {
                RUNTIME_TRACE_CALLBACK(USession, Receive, session->GetRuntimeTraceID(), readSequence);
                session->CombinePacket(error, receivedBytes);
            }));
    });
}

void USession::Send(const void* data, uint64 packetSize, uint16 packetType)
{
    RUNTIME_TRACE_SCOPE(USession, Send, GetRuntimeTraceID());
    const auto framedBuffer = MakePacketBuffer(data, packetSize, packetType);
    if (!framedBuffer)
        return;

    SendShared(framedBuffer);
}

void USession::SendShared(const SHPTR<const BUFFER>& framedBuffer)
{
    RUNTIME_TRACE_SCOPE(USession, SendShared, GetRuntimeTraceID());
    if (!framedBuffer || framedBuffer->empty() || _Closed.load(std::memory_order_acquire))
        return;

    // 소켓은 여러 스레드에서 동시에 사용할 수 없으므로 대기열과 송신 시작을 _Strand에서 처리한다.
    // 게시한 작업이 실행될 때까지 Session을 유지한다.
    const auto submission = RUNTIME_TRACE_EVENT(Submit, USession, SendShared, GetRuntimeTraceID());
    ASIO::post(_Strand, [session = ThisShared<USession>(), framedBuffer, submission]() mutable {
        RUNTIME_TRACE_CALLBACK(USession, SendShared, session->GetRuntimeTraceID(), submission);
        if (!session->_Socket.is_open() || session->_Closed.load(std::memory_order_relaxed))
            return;

        const uint64 frameBytes{ static_cast<uint64>(framedBuffer->size()) };
        if (frameBytes > SESSION_MAX_SEND_QUEUE_BYTES - session->_QueuedSendBytes) {
            const auto coreInstance = session->GetCoreInstance();
            const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
            if (logger) {
                FString message{ "USession send queue exceeded limit (" };
                message << std::to_string(session->_QueuedSendBytes + frameBytes) << '/'
                        << std::to_string(SESSION_MAX_SEND_QUEUE_BYTES) << " bytes); closing slow connection";
                logger->TryLog(ELogLevel::Warning, message.View());
            }
            session->CloseConnection();
            return;
        }

        session->_PendingSendQueue.push_back(std::move(framedBuffer));
        session->_QueuedSendBytes += frameBytes;
        if (session->_SendingBatch.empty())
            session->StartSend();
    });
}

void USession::Close()
{
    RUNTIME_TRACE_SCOPE(USession, Close, GetRuntimeTraceID());
    // 소켓과 송신 대기열은 _Strand에서만 사용하므로 다른 스레드의 호출은 _Strand로 넘긴다.
    const auto submission = RUNTIME_TRACE_EVENT(Submit, USession, Close, GetRuntimeTraceID());
    ASIO::dispatch(_Strand, [session = ThisShared<USession>(), submission] {
        RUNTIME_TRACE_CALLBACK(USession, Close, session->GetRuntimeTraceID(), submission);
        session->CloseConnection();
    });
}

bool USession::PostToSession(FUNC<void()> task)
{
    RUNTIME_TRACE_SCOPE(USession, PostToSession, GetRuntimeTraceID());
    const auto session = ThisWeak<USession>().lock();
    if (!session || !task)
        return false;

    const auto submission = RUNTIME_TRACE_EVENT(Submit, USession, PostToSession, GetRuntimeTraceID());
    ASIO::post(_Strand, [session, task = std::move(task), submission]() mutable {
        RUNTIME_TRACE_CALLBACK(USession, PostToSession, session->GetRuntimeTraceID(), submission);
        task();
    });
    return true;
}

void USession::OnDisconnected()
{
}

void USession::StartSend()
{
    RUNTIME_TRACE_SCOPE(USession, StartSend, GetRuntimeTraceID());
    _SendingBatch.clear();
    _SendingBufferView.clear();

    while (!_PendingSendQueue.empty() && _SendingBatch.size() < SESSION_MAX_WRITE_BATCH_COUNT) {
        SHPTR<const BUFFER> frame = std::move(_PendingSendQueue.front());
        _PendingSendQueue.pop_front();
        _SendingBufferView.emplace_back(ASIO::buffer(frame->data(), frame->size()));
        _SendingBatch.push_back(std::move(frame));
    }

    if (_SendingBatch.empty())
        return;

    const auto submission = RUNTIME_TRACE_EVENT(Submit, USession, StartSend, GetRuntimeTraceID());
    ASIO::async_write(
        _Socket,
        _SendingBufferView,
        ASIO::bind_executor(_Strand, [session = ThisShared<USession>(), submission](const BOOST_ERROR_CODE& error, uint64) {
            RUNTIME_TRACE_CALLBACK(USession, StartSend, session->GetRuntimeTraceID(), submission);
            session->OnSend(error);
        }));
}

void USession::OnSend(const BOOST_ERROR_CODE& error)
{
    RUNTIME_TRACE_SCOPE(USession, OnSend, GetRuntimeTraceID());
    uint64 sentBytes{ 0 };
    for (const SHPTR<const BUFFER>& frame : _SendingBatch)
        sentBytes += static_cast<uint64>(frame->size());

    _SendingBatch.clear();
    _SendingBufferView.clear();
    _QueuedSendBytes = (_QueuedSendBytes > sentBytes) ? (_QueuedSendBytes - sentBytes) : 0;

    if (error) {
        // 쓰기가 끝난 뒤라 대기열 버퍼를 사용하는 작업이 없다. 닫는 소켓으로는 보낼 수 없으므로 남은 버퍼를 버린다.
        _PendingSendQueue.clear();
        _QueuedSendBytes = 0;
        CloseConnection();
        return;
    }

    if (!_PendingSendQueue.empty() && _Socket.is_open())
        StartSend();
}

void USession::CloseConnection()
{
    RUNTIME_TRACE_SCOPE(USession, CloseConnection, GetRuntimeTraceID());
    _Closed.store(true, std::memory_order_release);

    // 아직 전송 시작 전인 대기 버퍼만 즉시 비운다. 전송 중인 _SendingBatch는 취소 완료(OnSend)까지 유지한다.
    uint64 droppedBytes{ 0 };
    for (const SHPTR<const BUFFER>& frame : _PendingSendQueue)
        droppedBytes += static_cast<uint64>(frame->size());
    _PendingSendQueue.clear();
    _QueuedSendBytes = (_QueuedSendBytes > droppedBytes) ? (_QueuedSendBytes - droppedBytes) : 0;

    // 진행 중인 수신·송신은 취소되며 오류와 함께 완료 처리가 실행된다. 이미 닫힌 소켓은 오류 없이 넘어간다.
    BOOST_ERROR_CODE error;
    _Socket.close(error);
    const auto coreInstance = GetCoreInstance();
    if (error) {
        const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
        if (logger)
            logger->TryLogError("USession::Close failed", error);
    }

    // 접속 처리가 Receive 전에 추가를 게시하므로 이 제거는 추가 뒤에 실행된다. 여러 번 요청해도 된다.
    const auto networkManager = coreInstance ? coreInstance->GetNetworkManager() : nullptr;
    if (networkManager)
        networkManager->RequestRemoveSession(ThisShared<USession>());

    if (!_DisconnectedNotified) {
        _DisconnectedNotified = true;
        OnDisconnected();
    }
}

void USession::CloseOnProcessingFailure(const STRINGVIEW reason)
{
    RUNTIME_TRACE_SCOPE(USession, CloseOnProcessingFailure, GetRuntimeTraceID());
    // 먼저 닫아서 로그 문장을 만들다 할당 예외가 나도 연결은 정리되게 한다.
    CloseConnection();

    const auto coreInstance = GetCoreInstance();
    const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
    if (!logger)
        return;
    FString message{ "USession::ProcessingData failed: " };
    message << reason;
    logger->TryLog(ELogLevel::Error, message.View());
}

void USession::CombinePacket(const BOOST_ERROR_CODE& error, uint64 receivedBytes)
{
    RUNTIME_TRACE_SCOPE(USession, CombinePacket, GetRuntimeTraceID());
    _ReceivedBytes += receivedBytes;
    uint64 processedBytes{ 0 };
    bool validPacketSize{ true };

    // ProcessingData에서 Close하면 소켓이 바로 닫히므로, 같은 수신에 남은 패킷은 처리하지 않는다.
    while (_Socket.is_open() && _ReceivedBytes - processedBytes >= PACKET_HEADER_SIZE) {
        FPacketHeader header{};
        std::memcpy(&header, _ReceiveBuffer.data() + processedBytes, PACKET_HEADER_SIZE);

        if (header.size > PACKET_MAX_BODY_SIZE) {
            const auto coreInstance = GetCoreInstance();
            const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
            if (logger) {
                FString message{ "USession rejected oversized packet header (type=" };
                message << std::to_string(header.type) << ", size=" << std::to_string(header.size)
                        << ", max=" << std::to_string(PACKET_MAX_BODY_SIZE) << "); closing connection";
                logger->TryLog(ELogLevel::Warning, message.View());
            }
            validPacketSize = false;
            break;
        }

        const uint64 packetSize{ PACKET_HEADER_SIZE + header.size };
        if (_ReceivedBytes - processedBytes < packetSize)
            break;

        // 파생 클래스의 처리 실패는 이 연결만 끊는다. 예외가 워커 밖으로 나가면 서버 전체가 종료된다.
        // 닫은 Session은 다시 수신하지 않으므로 수신 버퍼를 정리하지 않고 반환한다.
        try {
            ProcessingData(_ReceiveBuffer.data() + processedBytes, packetSize);
        }
        catch (const std::exception& exception) {
            CloseOnProcessingFailure(exception.what());
            return;
        }
        catch (...) {
            CloseOnProcessingFailure("unknown exception");
            return;
        }
        processedBytes += packetSize;
    }

    if (processedBytes != 0) {
        _ReceivedBytes -= processedBytes;
        if (_ReceivedBytes != 0)
            std::memmove(_ReceiveBuffer.data(), _ReceiveBuffer.data() + processedBytes, _ReceivedBytes);
    }

    if (_Socket.is_open() && !error && receivedBytes != 0 && validPacketSize)
        Receive();
    else
        CloseConnection();
}

SPACE_T
