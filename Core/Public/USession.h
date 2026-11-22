#pragma once

#include "CoreDefine.h"
#include "UComponent.h"
#include "Packet.h"
#include <boost/asio/strand.hpp>

SPACE_H(Core)

// 세션당 송신 대기열 누적 상한(느린 클라이언트 OOM 방어), 1회 Scatter-Gather 배치 최대 건수, 기본 유휴 타임아웃이다.
STATIC_CONSTEXPR uint64 SESSION_MAX_SEND_QUEUE_BYTES{ 1024ull * 1024ull };
STATIC_CONSTEXPR uint64 SESSION_MAX_WRITE_BATCH_COUNT{ 64 };
STATIC_CONSTEXPR MILLISECONDS SESSION_DEFAULT_IDLE_TIMEOUT{ 30000 };

// UActor에 장착되는 TCP 통신 컴포넌트다.
// 바이트 스트림을 [4B 헤더 + 본문]으로 조립해 SafeGuard(FPacketView)로 소유자 UActor::OnReceivePacket에 전달하며,
// 송신 배칭 · 1 MiB 백프레셔 · 유휴 타임아웃을 담당한다. 상위 프로젝트는 UServerSession 등으로 확장할 수 있다.
class CORE_DLL USession : public UComponent {
public:
    explicit USession(SHPTR<UCoreInstance> coreInstance, TCPSOCKET socket);
    virtual ~USession() override;
    NO_COPY(USession)

    STATIC_CONSTEXPR uint32 ComponentTypeID{ static_cast<uint32>(EComponentType::Session) };

    // 세션 컴포넌트의 고정 유형 ID를 돌려준다.
    uint32 GetComponentTypeID() const noexcept final override { return ComponentTypeID; }

    // 파생 세션(Server::UServerSession 등)을 RTTI 없이 안전하게 판별하기 위한 고정 유형 ID다.
    virtual uint32 GetSessionTypeID() const noexcept { return 0; }

    // 헤더와 본문을 하나의 불변 송신 프레임으로 조립한다. 방 브로드캐스트처럼 같은 패킷을 여러 세션에 보낼 때
    // 한 번만 생성해 SendShared로 공유하면 수신자 수만큼의 메모리 할당과 복사를 없앨 수 있다.
    // packetSize가 PACKET_MAX_BODY_SIZE를 넘거나 본문이 있는데 data가 비어 있으면 빈 포인터를 돌려준다.
    static SHPTR<const BUFFER> MakePacketBuffer(const void* data, uint64 packetSize, uint16 packetType);

    // 어느 스레드에서나 호출할 수 있다. Receive 시작 전에 유휴 수신 타임아웃을 설정한다(0ms이면 비활성화).
    // 지정 시간 동안 수신 데이터가 전혀 없으면 Warning을 남기고 이 Session의 strand에서 연결을 닫는다.
    void SetIdleTimeout(MILLISECONDS timeout) noexcept;

    // 같은 버퍼를 사용하므로 수신 완료 처리 전에 중복 호출하지 않는다.
    // 어느 스레드에서나 호출할 수 있다. 수신 시작과 완료 처리는 이 Session의 strand에서 실행되며,
    // UNetworkManager::RequestAddSession은 Receive보다 먼저 호출한다.
    void Receive();

    // data와 packetSize는 헤더를 제외한 본문과 본문 크기다. data는 읽기만 하며 호출 중에 복사한다.
    // 어느 스레드에서나 호출할 수 있다. 버퍼는 이 Session의 strand에서 송신 대기열에 넣고 순서대로 보낸다.
    void Send(const void* data, uint64 packetSize, uint16 packetType);

    // MakePacketBuffer로 미리 조립한 불변 송신 프레임을 추가 복사 없이 송신 대기열에 넣는다.
    // 어느 스레드에서나 호출할 수 있으며 이미 닫힌 세션이면 즉시 무시한다.
    // 대기열 누적 바이트가 SESSION_MAX_SEND_QUEUE_BYTES를 넘으면 Warning 기록 후 연결을 닫는다.
    void SendShared(const SHPTR<const BUFFER>& framedBuffer);

    // 어느 스레드에서나 호출할 수 있다. 실제 처리는 이 Session의 strand에서 실행한다.
    // 소켓을 닫아 진행 중인 수신·송신을 취소하고, UNetworkManager 목록 제거와 소유자 OnSessionDisconnected를 1회 호출한다.
    void Close();

    // 어느 스레드에서나 호출할 수 있다. task를 이 Session의 strand에 게시하며, 실행이 끝날 때까지 Session을 유지한다.
    // Session이 shared_ptr 소유가 아니거나 task가 비어 있으면 false다.
    bool PostToSession(FUNC<void()> task);

private:
    // 완성된 SafeGuard 패킷 뷰를 소유자 UActor에 전달한다(이 Session의 strand 전용).
    void ProcessingData(const FPacketView& packet);
    // 이 Session의 strand에서 호출한다. 대기열에서 최대 SESSION_MAX_WRITE_BATCH_COUNT개를 모아
    // Scatter-Gather async_write 한 번으로 보낸다.
    void StartSend();
    // 이 Session의 strand에서 실행되는 송신 완료 처리다.
    void OnSend(const BOOST_ERROR_CODE& error);
    // 이 Session의 strand에서 호출하는 Close의 실제 처리다. 여러 번 호출해도 된다.
    void CloseConnection();
    // 이 Session의 strand에서 ProcessingData가 던진 예외를 받았을 때 호출한다. 연결을 닫고 reason을 Error로 기록한다.
    void CloseOnProcessingFailure(STRINGVIEW reason);
    // 이 Session의 strand에서 호출한다. 유휴 타임아웃이 설정돼 있으면 타이머를 현재 시점 기준으로 재설정한다.
    void ResetIdleTimerOnStrand();
    // 이 Session의 strand에서 호출한다. 진행 중인 유휴 타임아웃 타이머를 취소한다.
    void CancelIdleTimerOnStrand() noexcept;
    void CombinePacket(const BOOST_ERROR_CODE& error, uint64 receivedBytes);

    TCPSOCKET                               _Socket;
    ASIO::strand<ASIO::any_io_executor>     _Strand;
    BOOST_STEADY_TIMER                      _IdleTimer;
    ATOMIC_UINT64                           _IdleTimeoutMs{ 0 };
    uint64                                  _IdleGeneration{ 0 }; // 이 Session의 strand에서만 접근한다.
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
