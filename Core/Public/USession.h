#pragma once

#include "CoreDefine.h"
#include "UObject.h"
#include "IHashable.h"
#include "Packet.h"
#include <boost/asio/strand.hpp>

SPACE_H(Core)

class URoom;

class CORE_DLL USession abstract : public UObject, public IHashable<USession> {
protected:
    explicit USession(SHPTR<UCoreInstance> coreInstance, TCPSOCKET socket);
    virtual ~USession() override;

public:
    // 세션은 객체 주소를 기준으로 식별한다.
    uint64 GetHash() const noexcept final override;
    bool IsEqual(const USession& other) const noexcept final override;

    // 이 Session의 strand에서만 호출한다. ProcessingData와 입장 결과 콜백이 이 strand에서 실행된다.
    // 현재 방이 없으면 빈 포인터다. 입장 결과를 기다리는 동안에는 예약한 방을 돌려주며,
    // 그 방의 참가 목록에는 아직 없을 수 있다. strand 밖에서 호출하면 예외를 던진다.
    SHPTR<URoom> GetRoom() const;

    // 어느 스레드에서나 호출할 수 있다. true는 게시 성공이며 입장 결과는 onComplete로 전달한다.
    // 이 Session의 strand에서 Room에 요청한 뒤 결과가 돌아오기 전에 방을 예약하므로,
    // 결과가 나오기 전의 다른 입장 요청은 실패한다. Room이 거절하면 예약을 지운다.
    // 닫힌 Session은 입장하지 못한다. 결과를 기다리는 동안 Close되면 Room 목록에서도 제거되며 실패로 알린다.
    // onComplete는 이 Session의 strand에서 실행된다.
    // Session과 room은 결과 전달까지 유지된다. 입장한 Session은 Close에서 Room 목록 제거를 요청한다.
    // 입장한 Session은 Close까지 살아 있어야 한다. 수신이 걸려 있으면 완료 처리가 Session을 유지하지만,
    // Receive 없이 입장한 뒤 참조를 모두 놓으면 Room 목록에 만료된 항목이 남아 정원을 차지한다.
    // 연결을 유지한 채 방만 나가는 퇴장은 아직 구현되지 않았다.
    // io_context가 종료되거나 중지되면 onComplete가 실행되지 않을 수 있다. 게시 예외는 호출자에게 전달한다.
    // 처리 중 할당이나 콜백 예외는 네트워크 워커의 오류 경로로 전달되며, 이때 onComplete는 보장되지 않고
    // 예약이 남을 수 있다.
    bool RequestEnterRoom(const SHPTR<URoom>& room, FUNC<void(bool)> onComplete);

    // 같은 버퍼를 사용하므로 수신 완료 처리 전에 중복 호출하지 않는다.
    // 수신 완료 처리는 이 Session의 strand에서 실행된다. 처음 호출은 strand 밖에서 소켓과 수신 상태를 쓰므로
    // Session을 다른 스레드에 공개하기 전, 즉 Send·RequestEnterRoom·Close 전에 한다.
    // 완료 처리가 Session을 붙잡으므로 수신이 걸려 있는 동안 Session이 유지된다.
    // shared_ptr 소유 상태에서 호출하며 아니면 std::bad_weak_ptr을 던진다.
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

    // 어느 스레드에서나 호출할 수 있다. 실제 처리는 이 Session의 strand에서 실행한다.
    // strand 안에서 부르거나, strand가 비어 있을 때 io_context를 실행 중인 워커에서 부르면 반환 전에 실행되며
    // 그때는 처리 중 예외도 호출자에게 전달된다. 그 밖에는 반환 뒤에 실행된다.
    // 소켓을 닫아 진행 중인 수신·송신을 취소하고, 현재 방(예약 포함)이 있으면 Room 목록 제거를 요청한 뒤 비운다.
    // ProcessingData 안에서 부르면 같은 수신에 남은 패킷은 처리하지 않는다.
    // 닫기 오류는 로거 기록을 시도한다. 여러 번 호출해도 된다.
    // UNetworkManager의 Session 목록 제거는 그 목록이 아직 없어서 하지 않는다.
    // shared_ptr 소유 상태에서 호출하며 아니면 std::bad_weak_ptr을 던진다. 게시 예외는 호출자에게 전달한다.
    // io_context가 종료되거나 중지되면 실행되지 않을 수 있다.
    void Close();

protected:
    // packetData는 헤더를 포함한 패킷 하나이고, packetSize는 헤더를 포함한 전체 크기다.
    // packetData는 수신 버퍼를 가리키므로 이 함수가 반환할 때까지만 사용한다.
    // 이 Session의 strand에서 호출되므로 GetRoom()을 바로 사용할 수 있다.
    // 예외를 던지면 네트워크 워커의 오류 경로로 전달되고 Session은 닫히지 않는다. 방에 든 Session이면
    // Room 목록 제거도 요청되지 않아, Session이 소멸한 뒤 만료된 항목이 정원을 차지한다.
    virtual void ProcessingData(uint8* packetData, uint64 packetSize) = 0;


private:
    // 이 Session의 strand에서 호출한다. Room에 참가 목록 추가를 요청하고 현재 방을 예약한다.
    // 닫힌 Session이거나 이미 방이 있으면 실패로 알린다.
    void ReserveRoom(const SHPTR<URoom>& room, FUNC<void(bool)> onComplete);
    // 이 Session의 strand에서 호출한다. Room이 거절했으면 예약을 지우고 결과를 전달한다.
    // 결과를 기다리는 동안 Close가 예약을 지웠으면 실패로 알린다.
    void FinishEnterRoom(const SHPTR<URoom>& room, bool entered, const FUNC<void(bool)>& onComplete);
    // 이 Session의 strand에서 호출한다. 대기열 앞 버퍼를 끝까지 보내며, 그 버퍼는 송신 완료까지 대기열에 남는다.
    void StartSend();
    // 이 Session의 strand에서 실행되는 송신 완료 처리다. 다음 버퍼를 보내고, 실패하면 남은 버퍼를 버리고 닫는다.
    void OnSend(const BOOST_ERROR_CODE& error);
    // 이 Session의 strand에서 호출하는 Close의 실제 처리다. 여러 번 호출해도 된다.
    void CloseConnection();
    void CombinePacket(const BOOST_ERROR_CODE& error, uint64 receivedBytes);

    WKPTR<URoom>                            _Room;
    TCPSOCKET                               _Socket;
    ASIO::strand<ASIO::any_io_executor>     _Strand;
    QUEUE<BUFFER>                           _SendQueue; // 이 Session의 strand에서만 사용한다. 앞 버퍼가 송신 중이다.
    PACKET_BUFFER                           _ReceiveBuffer{};
    uint64                                  _ReceivedBytes{ 0 }; // 수신 버퍼에 남아 있는 바이트 수
};

SPACE_T
