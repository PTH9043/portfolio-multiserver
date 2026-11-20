# 다중 접속 2D → 3D 게임

> **2026-11-08 현재 구조 안내:** 현재 솔루션에는 Core와 Server만 있으며 예전 GameClient2D 경로는 없습니다. 별도 클라이언트는 `../Client/ChatClient.csproj`의 빈 Windows Forms 프로젝트입니다. 아래의 과거 일정과 준비 보고는 당시 기록으로 보존하며, 현재 채팅 서버 우선순위와 AI 협업 계획은 [상위 작업 순서](../README.md)를 따릅니다.


## Core 갱신과 Protocol Buffers

- Core의 Boost 헤더는 `ThirdParty/boost_1_92_0`에 보관한 Boost 1.92.0을 사용합니다. Core와 Server의 `BoostIncludeDir` 빌드 속성으로 다른 Boost 헤더 경로를 지정할 수 있습니다. 이 폴더에는 공식 배포본의 헤더와 라이선스가 있으며 빌드된 Boost 라이브러리는 포함하지 않습니다.
- `Update_Core_Server.bat`을 실행하면 Core와 Server를 Debug x64로 빌드하고, Server 실행 폴더의 Core DLL이 빌드된 DLL과 같은지 검사합니다. 기존 Visual Studio 솔루션 빌드도 Core 프로젝트 참조와 DLL 복사를 수행합니다. Release는 명령 프롬프트에서 `.\Update_Core_Server.bat Release`로 실행합니다.
- 패킷 번호는 `Protocol/Schema/packet_type.proto`의 `ECSPacket`(클라→서버)·`ESCPacket`(서버→클라)에, 메시지는 영역별 파일(`login.proto`·`chat.proto`·`room.proto`)에 둡니다. 번호 0은 비워 두고, 사용했던 패킷 번호와 필드 번호는 다시 쓰지 마세요.
- `Build_Protocol.bat`을 실행하면 `.proto` 전체에서 C++(`Protocol/Generated/Cpp`)과 C#(`Protocol/Generated/CSharp`) 코드를 만들고 C++ `GameProtocol.lib`를 Debug x64로 빌드합니다. Release는 명령 프롬프트에서 `.\Build_Protocol.bat Release`입니다. 생성 폴더는 자동 생성 전용이므로 직접 수정하지 말고 `.proto`를 수정한 뒤 다시 실행하세요. Windows용 `protoc`의 경로 처리 때문에 `.proto` 파일과 하위 폴더 이름은 영문·숫자·밑줄을 사용하세요.
- C++ 서버는 `#include "login.pb.h"`처럼 영역별 생성 헤더로 메시지에 접근하며, 프로토콜 빌드가 완료된 같은 구성의 라이브러리를 사용합니다. C# 클라이언트는 `Google.Protobuf` 패키지와 생성된 C# 코드를 참조합니다. 생성 코드를 바꾼 뒤 각 프로젝트를 다시 빌드하세요.
- `.proto`를 수정한 뒤에는 사용하려는 구성(Debug 또는 Release)으로 `Build_Protocol.bat`을 다시 실행하세요. 새 정의에 비해 프로토콜 빌드가 오래된 경우 Server와 GameClient2D 빌드는 재생성을 안내하고 멈춥니다. 구성별로 각각 다시 빌드해야 합니다.
- 첫 실행에는 Git, Visual Studio의 C++ 도구와 CMake, Python, 인터넷이 필요합니다. `Protocol/vendor`와 `Protocol/build`는 소스 의존성과 빌드 결과를 보관합니다. 생성기는 공식 Google.Protobuf.Tools 3.33.6, C++ 런타임은 protobuf v33.6 소스를 사용합니다. 다른 버전으로 따로 교체하지 마세요.
- protobuf는 본문의 직렬화 형식입니다. TCP 메시지 경계는 기존 4바이트 머리말(`type`=패킷 번호, `size`=본문 크기)로 나눕니다. 서버의 패킷 해석·송신 처리는 아직 구현하지 않았습니다(결정 사항은 `AGENTS.md` 1-1절).

태현님이 AI에 질문하며 직접 구현하는 실제 프로젝트입니다. 1주일 동안 작은 2D 총게임을 목표로 만들고, 이후 3주 동안 3D 클라이언트로 확장합니다. 일정은 목표이며 미완료 항목은 기록합니다.

## 지금 시작하기

- 솔루션: MultiplayerGame.sln, Visual Studio의 Debug | x64
- 서버 · Core(DLL): Core/Public, Core/Private — 서버 네트워크 코드(리스너·세션·수신 버퍼·세션 관리)를 먼저 여기에 작성
- 서버 · Server(EXE): Server/Default/Main.cpp, Server/Private/ServerApp.cpp — Core를 사용해 패킷 처리와 게임 규칙만 작성, 소켓 함수 직접 호출 없음
- 클라 · C#: GameClient2D/Form1.cs — Core를 링크하지 않고 같은 패킷 형식만 맞춤, 통신 코드는 Form1과 분리한 클래스로 작성
- 서버와 클라이언트의 약속: 패킷 목록 문서(종류 번호, 필드 순서·크기). 2일 차에 작성
- GitHub 서버 교재: D:/__job/game-server-bootcamp-with-ai/02-phase2-network_kr.md (18일 차, 이후 21~22일 차)

Core/Server는 기존 ServerFramework 작업본의 복사이며 출처는 SOURCE_ORIGIN.json에 있습니다. Server의 프로젝트 설정은 이전 준비 단계에서 AI가 Core 프로젝트 참조와 DLL 배치를 연결했습니다. 클라이언트는 .NET Windows Forms 기본 프로젝트로 생성했습니다. 어떤 부분을 태현님이 직접 작성하고 AI가 도왔는지는 진행하면서 기록합니다.

2026-11-07 준비 검증: Debug x64 솔루션 빌드 성공. 2026-11-08 06시 재확인: 같은 빌드가 성공했고, 서버가 `Server Start` → Enter → `Server Stop` → `End Server`를 출력한 뒤 종료 코드 0으로 끝났습니다. 준비 이후 C++·C# 소스는 바뀌지 않았습니다. C++ 서버는 시작·종료 뼈대이고 클라이언트는 빈 창입니다. 접속·게임플레이가 이미 구현됐다는 뜻이 아닙니다.

기존 Core에서 참고할 수 있는 것: `Core/Public/Packet.h`의 `FPacketHeader`(`type` 2바이트 다음 `size` 2바이트, 리틀 엔디안, 최대 512바이트), 공통 기반 `UCoreBase`(서버 ID, `MakeShared`·`ThisWeak`), ID 발급기 `UIDGenerator`, 전역 객체 `UCoreGlobal`, `UJobQueue`. `UPacket` 선언은 제거했으며 `Core/Private/Packet.cpp`에는 아직 구현이 없습니다. `UThreadManager`와 `UCoreInstance`는 재설계를 위해 빈 틀만 남아 있습니다. Core에는 아직 소켓 코드가 없습니다.

전체 일정과 날짜별 완료 기준: [30일 계획표](../../docs/03_3일_계획과_구조도.html) (2026-11-08 점검·수정)

## 1~3일 (11/8~11/10): 패킷 서버 목표

3일 목표는 간단한 서버·클라이언트의 패킷 송수신입니다. 네트워크 게임 전체가 3일에 완성된다는 뜻은 아닙니다. 기간은 예상이며 미완료는 기록합니다. C# 통신 코드는 1일 차부터 `Form1`과 분리된 클래스로 작성해 3일 차 자동 시험, 2D, Unity를 고를 경우 3D에서 다시 씁니다.

1일 (11/8), 교재 Phase 2 18일 차(`L2-CPP-01`, `L2-C-01`)
- 서버 · Core: `winsock2.h` 포함 순서와 `Ws2_32.lib` 연결, Winsock 초기화·정리, 리스너(`bind`·`listen`·접속 스레드), 세션(`ACoreBase` 상속, 수신 스레드, 부분 송신 반복, `recv` 0이면 종료), Server에 접속·수신·종료를 알리는 방식 결정
- 서버 · Server: `ServerApp::Start()`·`Stop()`은 Core 리스너 시작·정지만 호출, 에코 처리
- 클라 · C#: 통신 클래스(접속·수신 스레드·송신), `Form1` 임시 입력칸
- 완료 기준: 문자열 왕복, `ServerApp.cpp`에 소켓 함수 호출 없음, Enter 종료 시 종료 코드 0과 누수 보고 없음

2일 (11/9), 교재 20일 차, 21~22일 차(`L2-C-07`·`08`·`10`)
- 서버 · Core: 세션 수신 버퍼로 기존 `FPacketHeader` 기준 분리, 완전한 패킷만 Server에 전달, `size`를 몸통 크기로 해석, 크기 초과 시 그 세션만 종료, 송수신을 공통 세션에서 담당하는 방향 검토
- 서버 · Server: 패킷 종류별 처리 함수, 패킷 목록 문서 작성
- 클라 · C#: 같은 머리말 순서·리틀 엔디안으로 쓰기·읽기, 분할·병합 시험용 송신
- 완료 기준: 분할·병합 수신을 각각 처리, 크기 초과 시 그 연결만 종료, 패킷 목록 문서 있음

3일 (11/10), 교재 26·31일 차, `L2-C-19`
- 서버 · Core: 세션 관리(`UIDGenerator` ID, `mutex` 보호 목록, 전체 전송), 종료 순서(새 접속 차단 → 세션 닫기 → 스레드 정리)
- 서버 · Server: 입장·퇴장 알림, 채팅 전체 전송
- 클라 · C#: 입장·퇴장·채팅 표시, 자동 시험 모드 10개
- 완료 기준: 10개 접속과 상호 수신, 하나씩 끊어도 서버 유지. 연결만 확인한 경우와 데이터 왕복 성공은 구분해 기록

## 4~7일 (11/11~11/14): 2D 게임 목표

게임 규칙은 Server에, 화면·입력은 C#에 둡니다. 4일 차부터 Core는 고치지 않는 것이 목표이며, 고쳤다면 이유를 기록합니다.

4일 (11/11): 서버 — 플레이어 목록·위치, 게임 루프(예: 20Hz) 한 스레드만 상태 변경, 기존 `JobQueue` 사용 가능 여부 확인, 방향만 받아 속도 제한 후 전체 전송. 클라 — 방향키 이동 요청, `BeginInvoke`로 UI 전달, 임시 도형 그리기. 교재 Phase 2 28일 차, Phase 3 37·38일 차, 심화 3-3b.
5일 (11/12): 서버 — 명중 방식 결정, 발사·피해·탄약·장전 판정. 클라 — 마우스 방향 발사, 장전 키, 체력·탄약 표시. 교재 Phase 3 44일 차.
6일 (11/13): 서버 — 아이템 1종, 사망, 최후 생존자, 대기 → 진행 → 종료. 클라 — 아이템·사망·승자 표시. 교재 Phase 3 39일 차.
7일 (11/14): 서버 — 끊김·잘못된 패킷 시험, Core·Server 경계 점검. 클라 — 자동 시험 모드 10개로 이동·발사 시험, 3D 엔진 실행 확인과 결정. 늦어지면 아이템을 먼저 뺍니다.

선택한 수치: 체력 100, 탄창 10발, 피해 25, 장전 2초. 10명은 첫 시험 목표이며 실제 10인 원격 접속을 검증했다는 뜻이 아닙니다. 명중 방식은 5일 차 시작, 아이템 종류는 6일 차 시작, 3D 엔진은 7일 차에 정합니다.

## 8~28일 (11/15~12/5): 3D 확장 목표

서버 판정은 2D와 같은 평면 좌표를 그대로 쓰고, 3D에서는 높이를 표시에만 씁니다. 점프·높이 판정·물리는 넣지 않습니다. 각 주의 토요일(14·21·28일 차)은 예비일입니다.

서버는 그대로 두고 클라이언트만 3D로 새로 만드는 것이 목표입니다. 4주 차에 서버 대기 주소를 실행 인자로 바꿀 수 있게 하고(127.0.0.1 / 0.0.0.0) 다른 PC 접속을 시험합니다.

2주 차 (11/15~11/21): 평면 맵·임시 캐릭터·카메라, 통신 연결, 여러 플레이어 이동과 보간. 완료 기준: 3D 클라이언트 2개와 2D 클라이언트 1개가 같은 서버에서 서로의 이동을 봅니다.
3주 차 (11/22~11/28): 조준·발사, 체력·탄약·장전 UI, 피격·사망 표시.
4주 차 (11/29~12/5): 아이템·승리, 자동 클라이언트와 3D 혼합 시험, 가능하면 같은 네트워크의 다른 PC 접속, 시연 영상과 실행 방법.
29~30일 (12/6~12/7): 미해결 오류와 실측 결과 정리, 지원·면접 자료 보완.

Unity는 과거 라이선스 관련 오류가 보고되었고 이번에 3D 빌드는 검증하지 않았습니다. Windows Forms 화면 코드가 Unity에서 자동으로 동작하는 것은 아니며 클라이언트 연결과 화면을 별도로 구현하고 시험해야 합니다.

개발·서류 작업과 지원·이론/면접은 09:00~21:00 중 각각 5시간·2시간·3시간을 목표로 병행하고 식사·휴식 2시간을 둡니다. 30일 동안 IOCP 전환, 메모리 풀·작업 큐의 새 기능, C#에서 C++ Core 호출, DB·로그인·재접속은 하지 않습니다.


## Core 회귀 시험 (2026-11-13)

Visual Studio C++ 도구(v145)와 프로젝트의 `ThirdParty/boost_1_92_0`이 필요합니다. 개발자 명령 프롬프트에서 프로젝트 폴더를 기준으로 실행합니다. `Debug` 대신 `Release`를 지정할 수 있습니다.

```bat
msbuild Tests\MemoryContracts.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
Tests\.build\Debug\bin\MemoryContracts.exe
msbuild Tests\LoggerContracts.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
Tests\.build\LoggerContracts\Debug\bin\LoggerContracts.exe
```

두 프로젝트는 필요한 현재 Core 소스를 시험 실행 파일에 직접 컴파일합니다. 기존 서버 실행 파일이나 Core DLL을 교체하지 않으며 DLL ABI 호환성은 시험하지 않습니다. `MemoryContracts`는 할당 경계·정렬·재사용·다중 스레드와 다른 스레드에서 반환을 확인합니다. `LoggerContracts`는 채널의 64건·100ms 경계, 종료 전 부분 묶음 출력, 두 생산자의 메시지 누락·중복과 생산 완료 후 종료 수거를 확인합니다. 비동기 출력 시험의 3초는 시험 제한 시간이며 100ms 내 출력 보장은 아닙니다.

실제 디스크 장애, OS 할당 실패, 네트워크 부하는 시험 범위가 아닙니다. 기존 `MemoryWalkthrough`와 `ThreadSchedulerTests`는 과거 API와 삭제된 소스를 참조하는 별도 오래된 프로젝트이며 위 시험에 포함되지 않습니다.

## 리스너 회귀 시험 (2026-11-13)

`UListener`는 별도 접속 수락 추상 클래스입니다. 주소와 포트는 호출자가 전달하고, 수락 소켓은 파생 클래스의 `HandleAcceptedSocket(TCPSOCKET)`으로 이동해 전달합니다. 수락 오류는 오류 콜백을 호출한 후 중단하며 종료 요청에 따른 취소는 오류로 전달하지 않습니다.

```bat
msbuild Tests\ListenerContracts.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
Tests\.build\ListenerContracts\Debug\bin\ListenerContracts.exe
```

Release 시험은 두 명령의 Debug를 Release로 바꿉니다. 시험 프로젝트는 Core 소스를 실행 파일에 직접 컴파일하며, 로컬 접속·포트 충돌·종료 경합·콜백 예외를 확인합니다. 시험 포트는 별도 수락기로 빈 포트를 확인한 후 사용하므로, 확인과 리스너의 바인딩 사이에 다른 프로세스가 그 포트를 선점할 가능성은 있습니다.

`Stop()`은 이미 전달 대상으로 확정되었거나 실행 중인 파생 함수와 실패 콜백이 끝날 때까지 기다리지 않습니다. 호출자는 이들이 참조하는 객체와 io_context를 관련 완료 핸들러 및 워커 종료까지 유지해야 합니다. Start()는 워커 실행 전 제어 스레드에서 호출합니다. 시작에 성공한 리스너 객체는 재시작하지 않습니다. `ServerApp`은 `ServerApp <네트워크 워커 수> <포트>`로 실행하며, `127.0.0.1:<포트>`에서 `UServerListener`로 접속을 받습니다. 접속마다 `UServerSession`을 만들어 `UNetworkManager`에 등록하고 수신을 시작합니다. 아직 정해진 패킷이 없어 받은 패킷은 Warning으로 기록하고 그 연결을 닫습니다. accept 오류가 나면 Error로 기록하고 서버를 종료합니다.
