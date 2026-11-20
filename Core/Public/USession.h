#pragma once

#include "CoreDefine.h"
#include "UObject.h"
#include "IHashable.h"
#include "Packet.h"
#include <boost/asio/strand.hpp>

SPACE_H(Core)

// 세션당 송신 대기열 누적 상한(느린 클라이언트 OOM 방어)과 1회 Scatter-Gather 배치 최대 건수다.
STATIC_CONSTEXPR uint64 SESSION_MAX_SEND_QUEUE_BYTES{ 1024ull * 1024ull };
STATIC_CONSTEXPR uint64 SESSION_MAX_WRITE_BATCH_COUNT{ 64 };

class CORE_DLL USession abstract : public UObject, public IHashable<USession> {
protected:
    explicit USession(SHPTR<UCoreInstance> coreInstance, TCPSOCKET socket);
    virtual ~USession() override;

public:
    // 헤더와 본문을 하나의 불변 송신 프레임으로 조립한다. 방 브로드캐스트처럼 같은 패킷을 여러 세션에 보낼 때
    // 한 번만 생성해 SendShared로 공유하면 수신자 수만큼의 메모리 할당과 복사를 없앨 수 있다.
    // packetSize가 PACKET_MAX_BODY_SIZE를 넘거나 본문이 있는데 data가 비어 있으면 빈 포인터를 돌려준다.
    static SHPTR<const BUFFER> MakePacketBuffer(const void* data, uint64 packetSize, uint16 packetType);

    // 세션은 객체 주소를 기준으로 식별한다.
    uint64 GetHash() const noexcept final override;
    bool IsEqual(const USession& other) const noexcept final override;

    // 같은 버퍼를 사용하므로 수신 완료 처리 전에 중복 호출하지 않는다.
    // 어느 스레드에서나 호출할 수 있다. 수신 시작과 완료 처리는 이 Session의 strand에서 실행되며,
    // strand 안이거나 strand가 비어 있을 때 io_context를 실행 중인 워커에서 부르면 반환 전에 시작한다.
    // 그 전에 Close됐으면 수신을 시작하지 않는다.
    // UNetworkManager::RequestAddSession은 Receive보다 먼저 호출한다. 그래야 Close의 목록 제거가 추가 뒤에 실행된다.
    // 완료 처리가 Session을 붙잡으므로 수신이 걸려 있는 동안 Session이 유지된다.
    // shared_ptr 소유 상태에서 호출하며 아니면 std::bad_weak_ptr을 던진다. 게시 예외는 호출자에게 전달하고,
    // strand에서 수신을 시작할 때의 예외는 반환 전에 시작한 경우 호출자에게, 아니면 네트워크 워커의 오류 경로로 전달된다.
    // io_context가 종료되거나 중지되면 수신을 시작하지 않을 수 있다.
    void Receive();

    // data와 packetSize는 헤더를 제외한 본문과 본문 크기다. data는 읽기만 하며 호출 중에 복사한다.
    // 어느 스레드에서나 호출할 수 있다. 버퍼는 이 Session의 strand에서 송신 대기열에 넣고 한 번에 하나씩 끝까지 보낸다.
    // 한 스레드에서 호출한 순서대로 보낸다. 게시한 작업과 송신 완료 처리가 Session을 붙잡으므로
    // 송신이 끝날 때까지 Session이 유지된다. 송신에 실패하면 남은 버퍼를 버리고 Close와 같은 정리를 한다.
    // shared_ptr 소유 상태에서 호출하며 아니면 std::bad_weak_ptr을 던진다. 버퍼 할당과 게시 예외는
    // 호출자에게 전달하고, strand에서 송신을 시작할 때의 예외는 네트워크 워커의 오류 경로로 전달된다.
    // 송신 시작 예외 뒤에는 대기열 앞 버퍼가 남아 이 Session의 송신이 더 진행되지 않고,
    // 이후 Send의 버퍼는 보내지 않은 채 Session이 소멸할 때까지 대기열에 쌓인다.
    // 크기가 PACKET_MAX_BODY_SIZE를 넘거나 본문이 있는데 data가 비어 있으면 보내지 않고 반환한다.
    // io_context가 종료되거나 중지되면 송신되지 않을 수 있다.
    void Send(const void* data, uint64 packetSize, uint16 packetType);

    // MakePacketBuffer로 미리 조립한 불변 송신 프레임을 추가 복사 없이 송신 대기열에 넣는다.
    // 어느 스레드에서나 호출할 수 있으며 이미 닫힌 세션이면 즉시 무시한다.
    // 대기열 누적 바이트가 SESSION_MAX_SEND_QUEUE_BYTES를 넘으면 Warning 기록 후 연결을 닫는다.
    void SendShared(const SHPTR<const BUFFER>& framedBuffer);

    // 어느 스레드에서나 호출할 수 있다. 실제 처리는 이 Session의 strand에서 실행한다.
    // strand 안에서 부르거나, strand가 비어 있을 때 io_context를 실행 중인 워커에서 부르면 반환 전에 실행되며
    // 그때는 처리 중 예외도 호출자에게 전달된다. 그 밖에는 반환 뒤에 실행된다.
    // 소켓을 닫아 진행 중인 수신·송신을 취소하고, UNetworkManager의 Session 목록 제거와 OnDisconnected를 호출한다.
    // ProcessingData 안에서 부르면 같은 수신에 남은 패킷은 처리하지 않는다.
    // 닫기 오류는 로거 기록을 시도한다. 여러 번 호출해도 된다.
    // shared_ptr 소유 상태에서 호출하며 아니면 std::bad_weak_ptr을 던진다. 게시 예외는 호출자에게 전달한다.
    // io_context가 종료되거나 중지되면 실행되지 않을 수 있다.
    void Close();

protected:
    // 어느 스레드에서나 호출할 수 있다. task를 이 Session의 strand에 게시하며, 실행이 끝날 때까지 Session을 유지한다.
    // Session이 shared_ptr 소유가 아니거나 task가 비어 있으면 false다. 게시 예외는 호출자에게 전달한다.
    bool PostToSession(FUNC<void()> task);

    // packetData는 헤더를 포함한 패킷 하나이고, packetSize는 헤더를 포함한 전체 크기다.
    // packetData는 수신 버퍼를 가리키므로 이 함수가 반환할 때까지만 사용한다.
    // 이 Session의 strand에서 호출된다.
    // 예외를 던지면 이 Session만 닫고 Logger에 Error로 기록한다. 서버와 다른 Session은 계속 동작한다.
    // 같은 수신에 남은 패킷은 처리하지 않는다.
    virtual void ProcessingData(uint8* packetData, uint64 packetSize) = 0;

    // 이 Session의 strand에서 연결이 닫힐 때 정확히 한 번 호출된다.
    // 파생 클래스가 소유한 게임 객체(예: UPlayer) 정리와 방 퇴장 후처리를 맡는다.
    virtual void OnDisconnected();


private:
    // 이 Session의 strand에서 호출한다. 대기열에서 최대 SESSION_MAX_WRITE_BATCH_COUNT개를 모아
    // Scatter-Gather async_write 한 번으로 보낸다. 전송 중인 버퍼들은 완료까지 _SendingBatch에 남는다.
    void StartSend();
    // 이 Session의 strand에서 실행되는 송신 완료 처리다. 다음 배치를 보내고, 실패하면 남은 버퍼를 버리고 닫는다.
    void OnSend(const BOOST_ERROR_CODE& error);
    // 이 Session의 strand에서 호출하는 Close의 실제 처리다. 여러 번 호출해도 된다.
    void CloseConnection();
    // 이 Session의 strand에서 ProcessingData가 던진 예외를 받았을 때 호출한다. 연결을 닫고 reason을 Error로 기록한다.
    // 닫기와 로그 문장 생성의 할당 예외는 호출자에게 전달한다.
    void CloseOnProcessingFailure(STRINGVIEW reason);
    void CombinePacket(const BOOST_ERROR_CODE& error, uint64 receivedBytes);

    TCPSOCKET                               _Socket;
    ASIO::strand<ASIO::any_io_executor>     _Strand;
    DEQUE<SHPTR<const BUFFER>>              _PendingSendQueue;     // 이 Session의 strand에서만 사용한다.
    VECTOR<SHPTR<const BUFFER>>             _SendingBatch;         // 현재 async_write 중인 불변 프레임 수명 유지.
    VECTOR<ASIO::const_buffer>              _SendingBufferView;    // Scatter-Gather 뷰 버퍼(재사용).
    uint64                                  _QueuedSendBytes{ 0 }; // 대기열 + 전송 중 누적 바이트 수.
    PACKET_BUFFER                           _ReceiveBuffer{};
    uint64                                  _ReceivedBytes{ 0 }; // 수신 버퍼에 남아 있는 바이트 수
    ATOMIC_BOOL                             _Closed{ false };    // 닫힌 소켓 빠른 거절용 원자 플래그
    bool                                    _DisconnectedNotified{ false }; // 이 Session의 strand에서만 접근한다.
};

SPACE_T
