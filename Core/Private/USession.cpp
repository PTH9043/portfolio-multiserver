#include "CoreDefine.h"
#include "USession.h"
#include "URoom.h"
#include "UCoreInstance.h"
#include "ULogger.h"
#include <boost/asio/bind_executor.hpp>
#include <boost/asio/buffer.hpp>
#include <boost/asio/dispatch.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/write.hpp>
#include <cstring>
#include <stdexcept>
#include <utility>

SPACE_H(Core)

USession::USession(SHPTR<UCoreInstance> coreInstance, TCPSOCKET socket)
    : UObject{ std::move(coreInstance) },
      _Socket{ std::move(socket) },
      _Strand{ ASIO::make_strand(_Socket.get_executor()) }
{
}

USession::~USession() = default;

uint64 USession::GetHash() const noexcept
{
    return static_cast<uint64>(std::hash<const USession*>{}(this));
}

bool USession::IsEqual(const USession& other) const noexcept
{
    return this == &other;
}

SHPTR<URoom> USession::GetRoom() const
{
    if (!_Strand.running_in_this_thread())
        throw std::logic_error("GetRoom requires the session strand");
    return _Room.lock();
}

bool USession::RequestEnterRoom(const SHPTR<URoom>& room, FUNC<void(bool)> onComplete)
{
    const auto session = ThisWeak<USession>().lock();
    if (!session || !room || !onComplete)
        return false;

    ASIO::post(_Strand, [session, room, onComplete = std::move(onComplete)]() mutable {
        session->ReserveRoom(room, std::move(onComplete));
    });
    return true;
}

void USession::Receive()
{
    // 완료 처리를 _Strand에서 실행해 ProcessingData와 입장 결과 처리가 _Room을 동시에 사용하지 않게 한다.
    // 완료 처리가 Session을 붙잡아 수신이 걸려 있는 동안 Session이 소멸하지 않게 한다.
    _Socket.async_receive(
        ASIO::buffer(_ReceiveBuffer.data() + _ReceivedBytes,
            _ReceiveBuffer.size() - _ReceivedBytes),
        ASIO::bind_executor(_Strand, [session = ThisShared<USession>()](const BOOST_ERROR_CODE& error, uint64 receivedBytes) {
            session->CombinePacket(error, receivedBytes);
        }));
}

void USession::Send(const void* data, uint64 packetSize, uint16 packetType)
{
    if (packetSize > PACKET_MAX_BODY_SIZE || (packetSize != 0 && data == nullptr))
        return;

    FPacketHeader header{};
    header.type = packetType;
    header.size = static_cast<uint16>(packetSize);

    BUFFER sendBuffer(PACKET_HEADER_SIZE + packetSize);
    std::memcpy(sendBuffer.data(), &header, PACKET_HEADER_SIZE);
    if (packetSize != 0)
        std::memcpy(sendBuffer.data() + PACKET_HEADER_SIZE, data, packetSize);

    // 소켓은 여러 스레드에서 동시에 사용할 수 없으므로 대기열과 송신 시작을 _Strand에서 처리한다.
    // 게시한 작업이 실행될 때까지 Session을 유지한다.
    ASIO::post(_Strand, [session = ThisShared<USession>(), sendBuffer = std::move(sendBuffer)]() mutable {
        // async_write가 끝나기 전에는 같은 소켓에 다른 쓰기를 시작하면 안 되므로 대기열이 비어 있었을 때만 시작한다.
        session->_SendQueue.push(std::move(sendBuffer));
        if (session->_SendQueue.size() == 1)
            session->StartSend();
    });
}

void USession::Close()
{
    // 소켓과 _Room은 _Strand에서만 사용하므로 다른 스레드의 호출은 _Strand로 넘긴다.
    ASIO::dispatch(_Strand, [session = ThisShared<USession>()] {
        session->CloseConnection();
    });
}

void USession::ReserveRoom(const SHPTR<URoom>& room, FUNC<void(bool)> onComplete)
{
    // 닫힌 뒤에 입장하면 Close의 정리가 이미 끝나 Room 목록에서 제거할 기회가 없다.
    if (!_Socket.is_open() || !_Room.expired()) {
        onComplete(false);
        return;
    }

    const auto session = ThisShared<USession>();
    const bool requested = room->RequestAddSession(session, [session, room, onComplete](bool entered) {
        // Room strand에서 실행된다. _Room은 이 Session의 strand에서만 바꾸므로 결과를 넘긴다.
        ASIO::post(session->_Strand, [session, room, entered, onComplete] {
            session->FinishEnterRoom(room, entered, onComplete);
        });
    });
    if (!requested) {
        onComplete(false);
        return;
    }

    // Room의 결과를 기다리는 동안 다른 입장 요청이 실패하도록 예약한다.
    // 결과는 이 strand로 돌아오므로 게시가 성공한 뒤 예약해도 결과보다 먼저 기록된다.
    _Room = room;
}

void USession::FinishEnterRoom(const SHPTR<URoom>& room, bool entered, const FUNC<void(bool)>& onComplete)
{
    // 결과를 기다리는 동안 Close가 예약을 지웠다면 Room 목록에서도 추가 뒤에 제거되므로 실패로 알린다.
    const bool reserved = _Room.lock() == room;

    // 거절되면 Room은 목록에 추가하지 않았으므로 Room 쪽에서 되돌릴 것은 없다.
    // 이 요청이 예약한 방일 때만 예약을 지운다.
    if (!entered && reserved)
        _Room.reset();

    onComplete(entered && reserved);
}

void USession::StartSend()
{
    // 앞 버퍼는 송신 완료까지 대기열에 남는다. 대기열 뒤에 버퍼를 넣어도 앞 버퍼의 데이터 주소는 바뀌지 않는다.
    const BUFFER& sendBuffer = _SendQueue.front();
    ASIO::async_write(
        _Socket,
        ASIO::buffer(sendBuffer.data(), sendBuffer.size()),
        ASIO::bind_executor(_Strand, [session = ThisShared<USession>()](const BOOST_ERROR_CODE& error, uint64) {
            session->OnSend(error);
        }));
}

void USession::OnSend(const BOOST_ERROR_CODE& error)
{
    if (error) {
        // 쓰기가 끝난 뒤라 대기열 버퍼를 사용하는 작업이 없다. 닫는 소켓으로는 보낼 수 없으므로 남은 버퍼를 버린다.
        _SendQueue = QUEUE<BUFFER>{};
        CloseConnection();
        return;
    }

    _SendQueue.pop();
    if (!_SendQueue.empty())
        StartSend();
}

void USession::CloseConnection()
{
    // 진행 중인 수신·송신은 취소되며 오류와 함께 완료 처리가 실행된다. 이미 닫힌 소켓은 오류 없이 넘어간다.
    BOOST_ERROR_CODE error;
    _Socket.close(error);
    if (error) {
        const auto coreInstance = GetCoreInstance();
        const auto logger = coreInstance ? coreInstance->GetLogger() : nullptr;
        if (logger)
            logger->TryLogError("USession::Close failed", error);
    }

    // 예약 중인 방도 포함한다. 추가 요청보다 나중에 게시하므로 Room에서는 추가 뒤에 제거된다.
    const auto room = _Room.lock();
    if (!room)
        return;

    // 게시가 예외로 실패하면 _Room을 남겨 다음 Close에서 다시 요청할 수 있게 한다.
    room->RequestRemoveSession(ThisShared<USession>());
    _Room.reset();
}

void USession::CombinePacket(const BOOST_ERROR_CODE& error, uint64 receivedBytes)
{
    _ReceivedBytes += receivedBytes;
    uint64 processedBytes{ 0 };
    bool validPacketSize{ true };

    // ProcessingData에서 Close하면 소켓이 바로 닫히므로, 같은 수신에 남은 패킷은 처리하지 않는다.
    while (_Socket.is_open() && _ReceivedBytes - processedBytes >= PACKET_HEADER_SIZE) {
        FPacketHeader header{};
        std::memcpy(&header, _ReceiveBuffer.data() + processedBytes, PACKET_HEADER_SIZE);

        if (header.size > PACKET_MAX_BODY_SIZE) {
            validPacketSize = false;
            break;
        }

        const uint64 packetSize{ PACKET_HEADER_SIZE + header.size };
        if (_ReceivedBytes - processedBytes < packetSize)
            break;

        ProcessingData(_ReceiveBuffer.data() + processedBytes, packetSize);
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
