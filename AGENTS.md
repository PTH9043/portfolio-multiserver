# MultiServer 코딩 규칙

이 문서는 네 부분으로 나눈다. 서로 충돌하면 **사용자의 최신 지시 > 0절·1절 > 2절 > 3절** 순서로 따른다. 단, 3절의 PowerShell 금지는 사용자의 공용 지시이므로 1절과 같은 무게로 지킨다.

0. **현재 프레임워크 먼저 파악**: 모든 AI, 특히 Codex가 코드를 읽거나 바꾸기 전에 반드시 거치는 절차다.
1. **사용자 규칙**: 사용자가 직접 말한 규칙과, 사용자의 지적을 받아 정리한 기준이다. 반드시 지킨다.
2. **이 솔루션의 코딩 규칙**: 현재 코드에서 확인한 관례다. 새 코드는 여기에 맞추되, 사용자가 명시한 규칙은 아니므로 바꿀 필요가 있으면 근거를 설명하고 사용자에게 확인한다.
3. **AI 작업 규칙**: Claude·Codex 등 AI가 이 솔루션의 코드를 작성·수정·검토할 때 지키는 절차다.

## 0. 현재 프레임워크 먼저 파악 (Codex 필수)

사용자는 Codex(`gpt-6.1-terra` 등)의 작업에 마음에 들지 않는 부분이 많다고 했고, 현재 프레임워크를 항상 파악한 뒤 작업하라고 지시했다(2026-11-14). 아래 절차는 분석만 요청받은 경우에도 적용한다.

**이 파일은 자동으로 읽히지 않을 수 있다.** Git 루트는 이 폴더지만(2026-11-15 도입) `D:\__job`에는 없다. Codex를 `D:\__job`에서 시작하면 그 폴더의 `AGENTS.md`만 자동으로 읽는다. 2026-11-14 확인한 MultiServer 작업 세션은 모두 `D:\__job`에서 시작했다. MultiServer 코드를 다루는 작업이면 시작할 때 이 파일 전체를 도구로 직접 연다.

### 0-1. 고치기 전에
1. 프레임워크의 기반 파일을 읽는다. `CoreDefine.h`, `CoreTypes.h`, `CoreStlTypes.h`, `CoreMacro.h`, `UCoreBase`·`UObject`·`UCoreInstance`가 그 대상이다. 작업 대상과 연결된 클래스도 읽는다(`UThreadManager`, `UNetworkManager`, `ULogger`, `FWorkerLogChannel`, `UListener`, `USession`, `ServerApp`, `UServerListener`, `UServerSession`).
2. 바꿀 함수의 호출 경로를 양쪽으로 따라간다. 그 함수를 누가 부르는지, 그 함수가 무엇을 부르는지 본다.
3. 같은 일을 이미 하는 함수·별칭·로그 경로·오류 전달 경로가 있는지 검색해서 찾는다. 사용자가 `GraduateProject`(`D:/__job/Project/GraduateProject/ServerFramework`) 같은 기존 구조를 언급하면 새 설계보다 먼저 그 코드를 읽는다.
4. 수정 전에 다음을 짧게 밝힌다: 대상 클래스의 책임, 재사용할 기존 기능, 바꿀 파일·함수, 이번에 하지 않을 일. 작은 수정이면 한두 줄로 충분하다.

### 0-2. 하지 말 것 (실제 사례)
- **기존 기능을 두고 같은 일을 하는 새 경로를 만들지 않는다.** Logger를 두고 넣은 `std::cerr` 출력(11-14), `BUFFER`를 직접 쓰면 되는데 만든 `AllocBuffer`(11-13), 이미 공통 수락 처리를 하는 `OnAccept`와 겹치는 `OnAccepted` 제안(11-13)이 그 예다.
- **요청하지 않은 함수·상태·클래스·정책을 넣지 않는다.** 세션 종료를 `BeginClose`·`CloseSocket`·`CheckCloseCompletion`·`FinishClose`로 쪼갠 것(사용자: "함수들 너무 많아"), ThreadManager에 로그 그룹과 토큰을 전제한 것이 그 예다.
- 반대로 함수 수만 줄이려고 처리를 긴 람다, 깊은 중첩, 긴 이름에 몰아넣지도 않는다(11-13).
- **사용자가 정한 순서를 앞지르지 않는다.** Close 처리를 할 차례에 세션 소유권 논의로 넘어간 것이 그 예다(사용자: "그 논의는 그 다음이야", 11-14).
- **오류를 버리지 않는다.** `ignored` 변수에 담고 끝내거나 `catch (...) {}`로 삼키지 않는다. 반대로 실제로 예외가 나지 않는 구간에 `try`를 두지 않는다. Logger 관련 `try`는 15개였다가 정리했다(11-14).
- **짝이 맞지 않는 코드를 남기지 않는다.** 선언만 있고 정의가 없는 함수, 만들고 등록하지 않은 워커가 그 예다. `RunNetworkWorker`는 만들고 `ThreadManager`에 등록하지 않았고(11-12), `USession::Initialize`는 선언만 있어서 Core DLL 링크가 실패했다(11-14 발견).
- 시험하려고 임시로 연결한 것을 실제 서버에 연결된 것처럼 보고하지 않는다.
- AI가 만든 `Tests/`의 시험에 맞추려고 사용자 API를 늘리거나 바꾸지 않는다.

### 0-3. 고친 뒤에
- 같은 일을 하는 코드가 둘이 되지 않았는지 다시 검색한다. 새 이름이 프로젝트 별칭·관례에 맞는지도 확인한다.
- 선언·정의·호출·등록의 짝을 확인한 뒤 3절의 방식으로 빌드한다. 하지 않은 빌드와 시험은 했다고 쓰지 않는다.
- 보고에는 다음을 적는다. 바꾼 파일과 함수, 이유, 사용자 선택과 AI 제안의 구분, 하지 않은 일.

## 1. 사용자 규칙

### 1-1. 사용자가 직접 말한 규칙

- `friend` 선언은 클래스·함수·템플릿을 포함해 전면 금지한다. 다른 클래스의 private 멤버에 접근하기 위해 `friend`를 사용하지 않는다. (2026-11-15 사용자 직접 지시: “friend 절대 금지야 코딩 규칙으로 적고”)
- `dynamic_cast`·`dynamic_pointer_cast`(RTTI) 대신 **`static_cast`·`static_pointer_cast` 위주**로 설계한다. 단, 맹목적 캐스팅으로 인한 UB를 막기 위해 반드시 **컴파일 타임 제약(`requires`·`static_assert`)** 또는 **정수 `TypeID`(`GetComponentTypeID`·`GetActorTypeID`·`GetRoomTypeID`) 일치 확인**을 거친 뒤 정적 캐스팅한다. (2026-11-22 사용자 직접 지시: “설계중에 dynamic_cast 위주로 말고 static 위주로 하기”)
- mutex 사용을 최대한 줄이고 lock-free 방식과 필요한 동기화를 함께 검토한다. 잠금을 추가하기 전에 실제 접근 경로와 동시성 보장 범위를 확인하고, 필요한 경우에만 기존 `SHARED_MUTEX` 및 용도에 맞는 잠금 별칭을 사용한다. 이름이나 컨테이너 종류만으로 lock-free라고 단정하지 않는다. (2026-11-09, 2026-11-15 사용자 직접 지시)

- 코드는 AI의 작성 편의가 아니라 **다른 사람이 읽고 이해하고 수정하기 쉬운 것**을 우선한다. (2026-11-14 사용자 지시: "다른 사람들이 보기 편하게 짜는걸 우선으로")
- 코딩의 원칙은 **책임 분리**다. 기능이 많다고 좋은 코드가 아니므로 필요한 기능부터 단순하게 만든다. (2026-11-09) 클래스마다 맡는 일을 먼저 설명하고, 한 번에 여러 클래스를 만들거나 설계를 임의로 확장하지 않으며, 필요한 함수와 변수부터 사용자와 함께 점검한다. (2026-11-08~10 대화)
- 프로젝트에 정의된 별칭을 사용한다. 예를 들어 `std::thread::id` 대신 `THREAD_ID`를 쓴다. (2026-11-09) `boost::asio` 대신 `ASIO`를 쓰고, `boost::asio`라는 이름은 `CoreTypes.h`의 `ASIO` 별칭 선언에만 둔다. (2026-11-13)
- `CoreTypes.h`, `CoreStlTypes.h` 등 프로젝트에 이미 정의된 `using` 타입·컨테이너·함수형·동기화 별칭을 모두 확인하고, 같은 타입과 의미·할당자·소유 방식을 나타내는 별칭이 있으면 반드시 사용한다. 크기와 개수, 해시 반환형 등에 `size_t`나 `std::size_t`를 직접 쓰지 말고 프로젝트의 `uint64`를 쓴다. OS·표준 라이브러리와 접하는 코드는 타입의 비트 폭·부호와 변환을 확인하고 기존 범위 검사와 호출 계약을 보존한다. (2026-11-15 사용자 직접 지시: “size_t -> 이런거 쓸거면 uint64를 써라 좀 using 을 모두 쓰게 지침 남겨”)
- 네트워크 타입도 `GraduateProject`에서 같은 타입에 사용한 공통 별칭을 따른다. `TCPSOCKET`과 `TCPACCEPTOR`를 사용한다. TCP endpoint의 별칭은 사용자가 정한 `TCP_ENDPOINT`를 사용한다. (2026-11-13)
- `UListener`는 추상 클래스다. 기존 `OnAccept()`는 공통 수락 완료 처리로 유지하며, 성공한 소켓은 사용자가 선택한 protected 순수 가상 함수 `HandleAcceptedSocket`에 넘긴다. 소켓은 기존 성공 콜백과 마찬가지로 `TCPSOCKET` 값으로 전달한다. 멤버 변수는 private으로 유지한다. 기존 실패 콜백은 유지했으며 장기 오류 전달 방식은 별도로 논의한다. (2026-11-13) 주 오류를 전달한 뒤 정리 단계에서 다시 난 close·Stop 오류도 버리지 않고, 공용 함수 `ULogger::TryLogError`로 기록한다. `USession::Close`도 같은 함수를 쓴다. (2026-11-14 사용자 선택: "공용 함수로 Logger 기록")
- class의 멤버 변수는 public으로 선언하지 않는다. (2026-11-10)
- 서버 접속 구조(2026-11-16 사용자 선택): `ServerApp <네트워크 워커 수> <포트>`로 실행하고 리스너는 `127.0.0.1`에서 연다. `UServerListener`가 수락한 소켓마다 공통 세션 `UServerSession` 하나를 만든다. 리스너의 accept 실패는 Error로 기록하고 네트워크 실패로 서버를 종료한다. 정해지지 않은 패킷은 Warning으로 기록하고 그 연결을 닫는다.
- 게임 사물·컴포넌트·패킷 전달 계층(2026-11-16, 2026-11-21~22 사용자 최종 확정): `Core.dll`에는 `UObject` 아래에 시스템/매니저, 공간(`URoom`), 부품(`UComponent → USession`), 사물(`UActor`) 4축만 두고 `UPawn`·`UCharacter`·Protobuf·MySQL을 넣지 않는다. `Core::UActor`가 컴포넌트 컨테이너(`CONUNORMAP<uint32, SHPTR<UComponent>>`)를 소유해 `USession`을 포함한 컴포넌트를 장착(`AddComponent`/`GetComponent`/`GetSession`)하고, `Core::USession`(`: Core::UComponent`, 상속 가능)은 바이트 프레임을 조립해 SafeGuard(`Core::FPacketView`, `NO_COPY` 제로카피 뷰 / 스레드 이동 시 `Detach()`)로 소유자 `UActor::OnReceivePacket`에 전달한다. `Server` 계층의 `UServerSession`(`: Core::USession`)은 도메인 파싱 없는 순수 전송 확장 클래스로 유지하고, `Server::UPawn`(`: Core::UActor`)이 공용 패킷(`HandlePacket`/`SendPacket`/`RejectPacket`/`CS_PING→SC_PONG`)을 한 곳에서 처리해 하위 `Server::UPlayer` 등으로 내려보낸다.
- 패킷 형식·SafeGuard(2026-11-19, 2026-11-22 사용자 선택 및 구현 완료): 머리말 `FPacketHeader` 뒤 본문은 Protocol Buffers로 직렬화한다. 패킷 번호는 `.proto` enum(`ECSPacket`·`ESCPacket`)으로 나누며 Core는 protobuf를 모른다. `USession::CombinePacket`은 `NO_COPY` 동기 뷰(`Core::FPacketView`)로 장착된 소유자 `UActor::OnReceivePacket`에 전달하고, 스레드·strand 경계를 넘겨야 할 때만 `Detach()`로 불변 `Core::FPacketPayload`(`SHPTR<const BUFFER>`)를 분리한다.
- 채팅·다중 방·유휴 타임아웃 범위(2026-11-19, 2026-11-22 사용자 선택 및 구현 완료): 고정 채널 4개(`1: 로비`, `2: 자유`, `3: 전략`, `4: 거래`, 각 1,000명)와 서버 전역 닉네임 유일성(`UNicknameRegistry`), 크로스-스트랜드 방 이동(`CS_ROOM_LIST`/`CS_JOIN_ROOM`), 하트비트(`CS_PING`/`SC_PONG`) 및 `USession` 30초 유휴 타임아웃(`--idle-timeout-ms`)을 지원한다. 닉네임은 2~12자 한글·영문·숫자, 채팅 본문은 200자까지이며 한도 초과 시 Warning 후 연결을 유지한다.
- MySQL 8 영속 계층 및 DB 전용 스레드 구조(2026-11-22 사용자 선택 및 구현 완료): 계정·인증·접속 이력 영속(`accounts`, `connection_logs`)은 `Server` 계층에만 두고 **`Core.dll`은 DB에 일절 의존하지 않는다.** 드라이버는 MariaDB Connector/C C API(`ThirdParty/mysql`, Schannel TLS + `caching_sha2_password` 정적 내장) 위에 얇은 RAII 래퍼(`FMySqlConnection`, `FMySqlStmtGuard`)를 두고 모든 쿼리에 Prepared Statement를 사용한다. 비밀번호는 Win32 CNG(`bcrypt.lib`) 16B 난수 솔트 + 32B PBKDF2-HMAC-SHA256 상수시간 비교로 검증한다. `UDatabaseManager`는 `UThreadManager::RegisterFunc`에 DB 전용 워커(`--db-workers=4`, 스레드당 전용 `MYSQL*` 1개, 풀 락 경합 0)를 등록하고 네트워크 워커 종료 → DB 큐 잔여 로그아웃 배수(`RequestStop`) → `ULogger` 종료의 3단 캐스케이드로 닫힌다. 실행 모드는 기본 `--db=mysql`이며, 기존 인메모리 회귀·기준선 비교용 `--db=off`를 함께 유지한다.
- 패킷 받기(2026-11-20 사용자 선택, 2026-11-22 `Server::UPawn` 단일화): 처리 함수를 패킷마다 만들지 않고 `Server::UPawn`의 공용 `HandlePacket<TPacket>` · `SendPacket<TPacket>` · `RejectPacket` · `CS_PING`으로 단일화했다. `OnDomainPacket`의 switch는 번호마다 메시지 타입만 고르고, 해석 실패와 받지 않는 번호는 `RejectPacket`이 Warning 후 연결을 닫는다. protobuf 이름은 `UPawn.cpp`·`UPlayer.cpp` 안에서만 `namespace Protocol = multiplayer::protocol;`로 줄여 쓴다.
- 로그의 적정선(2026-11-20 사용자 질문: "너무 많은 로그를 수집하려는거 아니야? 그 선을 잘 타야할것 같은데"): 운영자가 그 줄을 보고 할 일이 있는 것만 남긴다. 정상 흐름(패킷 수신 등)은 기록하지 않고 거절·실패만 남긴다. 지금 `HandlePacket`의 Info 기록은 임시이며 로그인·채팅 처리를 붙일 때 지운다. protobuf 내부(absl)의 stderr 출력은 우리 Warning과 중복이라 `Main.cpp`에서 `absl::InitializeLog()` 후 기준을 FATAL로 올려 끈다.
- 컨테이너 멤버 변수에 `_Sessions`처럼 복수형만 붙인 이름을 쓰지 않는다. `_SessionContainer`, `_SessionList`처럼 무엇을 담는 컨테이너인지 드러나게 짓는다. 같은 대상을 담는 기존 이름(`URoom::_SessionContainer`)이 있으면 그 이름에 맞춘다. (2026-11-16 사용자 직접 지시: "이런 코딩은 금지해 _Sessions라는 이름은 직관적이지 않아 -> 이런건 SessionContainer나 아니면 SessionList 같은걸로")
- 함수 이름은 결과의 보장 수준을 드러낸다. `Request…`는 요청만 하고 결과를 보장하지 않는 함수에 쓴다. 부르면 반드시 그 결과에 이르는 함수(예: 실패해도 강제로 끝내는 네트워크 종료 `UNetworkManager::Shutdown`)에는 `Request`를 붙이지 않는다. (2026-11-16 사용자 지시: "RequestStop은 요청한다잖아 부르면 꺼지면 강제로 꺼진다고 해야지")
- CPP에서 클래스 멤버 함수를 정의할 때 헤더에 선언된 순서를 따른다. (2026-11-12)
- 일반 `std::mutex` 사용을 금지한다. 동기화에는 프로젝트의 `SHARED_MUTEX`를 사용한다. 공유 상태를 읽기만 하고 동시 읽기가 안전한 구간은 `SHARED_LOCK_GUARD`를 사용하고, 상태 변경과 배타 처리가 필요한 구간은 `EXCLUSIVE_LOCK_GUARD`로 보호한다. (2026-11-11 원문: "읽기만 되는거면 READLOCK만 걸어 그냥 쌩 MUTEX는 전면 금지야")
- Core 객체와 매니저는 `UCoreBase` 계열을 상속하고, 매니저(`UThreadManager`, `UNetworkManager`, `ULogger`)는 `UCoreInstance`가 소유하며 매니저 기능은 `UCoreInstance`를 거쳐 사용하는 방향을 따른다(예: `RegisterWorkerThread`). 사용자는 2026-11-14에 `UNonCopyableObject`·`UCopyableObject` 중간 계층을 제거했다. 현재 `UThreadManager`·`UNetworkManager`·`ULogger`·`UListener`·`USession`은 `UObject`를 직접 상속하고, `UCoreInstance`와 `UJobQueue`는 `UCoreBase`를 직접 상속한다. `UObject`는 생성 시 받은 `UCoreInstance`를 `WKPTR`로 보관한다. `UObject` 자체에는 `NO_COPY`가 없으며, 복사·이동 가능 여부는 각 파생 클래스의 선언과 멤버를 확인한다.
  - 현재 이 방향과 다른 부분: `ServerApp`은 일부 매니저를 `Get…Manager()`로 받아 직접 호출한다. `FWorkerLogChannel`은 `MakeShared`로 만들지만 `UCoreBase`를 상속하지 않는다. 이런 부분을 바꿀지는 사용자가 정한다.

### 1-2. 사용자의 지적에서 정리한 기준

아래는 사용자가 AI의 실수를 지적한 뒤 AI가 정리해 이 문서에 남긴 기준이다. 사용자가 이 문장 그대로 말한 것은 아니지만, 같은 실수를 반복하지 않기 위해 1절로 다룬다.

- 변수와 함수 이름은 사용하는 사람이 문맥에서 뜻을 알기 쉬운 표현으로 짓는다. 불필요하게 긴 이름이나 낯선 약어를 피하고, 범위나 단위가 모호하면 짧은 주석으로 보완한다. 사용자가 정한 이름과 프로젝트 규칙은 그대로 따른다.
- 함수 분리와 통합은 사람이 흐름을 따라가기 쉬운지와 실제 책임을 기준으로 판단한다. 함수 개수만 줄이려고 긴 람다나 깊은 중첩에 처리를 몰아넣지 않고, 단순한 흐름도 불필요하게 잘게 나누지 않는다. 동작과 오류·수명 처리도 함께 확인한다.
- Core 코드를 작성할 때 `CoreTypes.h`, `CoreStlTypes.h`, `CoreMacro.h`에 이미 있는 타입과 매크로부터 확인한다. 같은 의미와 할당 방식을 가진 별칭(`SHPTR`, `UQPTR`, `WKPTR`, `FUNC`, `THREAD` 등)이 있으면 사용하고, 프로젝트 메모리 풀을 쓰는 객체는 `MakeShared`로 만든다. 사용자가 별도로 완화한 `ServerApp`의 제약은 그대로 둔다. (`THREAD_ID` 지적, 2026-11-09)
- 이름만 비슷하다고 타입을 무조건 바꾸지 않는다. 예를 들어 `THREADS`와 `VECTOR<THREAD>`는 할당자가 다르므로 소유권·할당·수명과 기존 계약을 확인한다. (같은 지적 후 분석, 2026-11-09)
- 사용자가 정하지 않은 재시작·오류 재시도·동시 호출·수명 정책은 이미 승인된 조건처럼 기록하거나 임의로 추가하지 않는다. 제안과 사용자의 선택, 현재 코드의 동작을 구분한다. (리스너 재시작 가정 지적, 2026-11-13)
- 잠금은 먼저 해당 상태를 실제로 동시에 누가 읽고 바꾸는지, 어떤 시점에 객체가 다른 스레드에 공개되는지 확인한 뒤 필요한 곳에 둔다. 워커 실행 전 제어 스레드만 접근한다고 보장된 초기화에, 근거 없이 실행 중 동시 호출을 가정해 잠금을 추가하지 않는다. 실행 중 수락 완료·종료·재시작 경합은 초기화 단계와 별도로 분석한다. (Acceptor 초기화 잠금 지적, 2026-11-13)
- `io_context`에 비동기 작업을 등록했다는 사실만으로 워커가 이미 돌고 있거나 아직 돌지 않는다고 가정하지 않는다. 호출 순서와 각 스레드의 수명·완료 핸들러의 대기 여부를 실제 호출 경로에서 확인한다. (같은 지적, 2026-11-13)
- 외부 라이브러리 객체가 동시 접근을 보장하지 않는 경우에는 읽기 함수라도 필요한 배타 잠금을 유지하고 이유를 주석으로 설명한다(예: Boost 수락기의 `local_endpoint`). (`SHARED_MUTEX` 전환, 2026-11-13)
- 조건 변수 대기에는 `COND_VARIABLE`(`condition_variable_any`)과 `std::unique_lock<SHARED_MUTEX>`를 사용한다. `EXCLUSIVE_LOCK_GUARD`는 `std::lock_guard`라 대기 중 잠금을 풀 수 없으므로 이 용도에 쓰지 못한다. 이 `std::unique_lock`은 대응하는 별칭이 없어 직접 표기를 허용한다. (`SHARED_MUTEX` 전환, 2026-11-13)
- 오류를 조용히 버리지 않고 로그로 남긴다. Logger를 거치지 않는 `std::cerr`·`std::cout` 우회 출력을 넣지 않는다. (2026-11-14 원문: "이건 왜 무시해 로그로 출력해야지", "Logger우리 열심히 했잖아 이건 뭐야? 이게 구조대로 맞아?")
  - AI가 정한 세부 기준(사용자 확인 전): 로그 스레드가 아직 돌지 않거나 종료 단계라 Logger 출력이 보장되지 않는 곳(`UServerApp::Start`·`RegisterWorkers`·`Stop`, `Main.cpp`의 종료 보고, 프로세스가 곧 끝나는 `UThreadManager`의 종료 처리기)만 콘솔에 직접 쓰고, 그 이유를 주석으로 남긴다.
- Logger에 오류가 생기면 멈추게 하고, `try`를 필요한 곳에만 쓴다. (2026-11-14 원문: "Logger에 에러가 생기면 멈추도록 해 야 코드가 너무 이상하잖아 try를 몇번 쓰는거야 적당히 해야지")
  - AI가 정한 세부 기준(사용자 확인 전): `try`/`catch`는 실제로 예외가 날 수 있는 호출(할당, `ASIO::post`·`async_*` 등록, 사용자 콜백, 스레드 생성)만 감싼다. MSVC의 `SHARED_MUTEX` 잠금·해제는 `noexcept`이므로 잠금만 하는 구간에는 두지 않는다. Logger 내부 오류는 접수를 멈추고 실패 콜백으로 종료를 요청한다. Logger의 실패 콜백(`Start`)은 예외를 던지지 않아야 한다. `Start`의 실패 콜백이 던지면 `noexcept` 경계에서 프로그램이 종료된다(2026-11-14 리팩토링 전에는 이 예외를 버렸다).
  - 현재 코드의 동작(2026-11-14 ULogger 정리): `post()` 실패도 Logger를 멈춘다. 그래서 `post()` 실패 뒤에도 Logger가 계속 돌던 때(2026-11-12 사용자가 고른 AI 제안)를 위해 둔 250ms 보조 수거 타이머와 `post()` 실패 횟수(`GetPostFailureCount`, `GetLoggerPostFailureCount`)는 할 일이 없어져 지웠다. 로그 수거는 `TryLog`마다 게시하는 수거 작업, 워커 채널 알림, 100ms 타이머 두 개(공용 묶음의 `_BatchTimer`, 채널 마감의 `_WorkerTimer`), 종료 때 `RunWorker`의 마지막 수거로 이뤄진다. `post()` 실패 뒤에도 Logger가 계속 도는 정책으로 되돌리면 보조 수거가 다시 필요하다.
  - 워커 채널(2026-11-14 사용자 선택: "남기고 낡은 부분 정리"): 서버는 아직 채널을 쓰지 않지만 11-12에 고른 워커별 로그 설계라 기능은 남겼다. "실패한 채널만 멈춤" 정책을 위해 있던 채널별 실패 콜백(`SetWorkerHandlers`의 둘째 인자, 지금은 `SetWorkerBatchHandler`)과 채널의 출력 실패 상태(`FinishOutput(bool)`)를 지웠다. 아무도 읽지 않던 `GetStatus`·거절 횟수·알림 순번도 지웠다. 채널 출력 콜백의 예외는 공용 큐 출력과 같이 `RunWorker`가 받아 Logger를 멈춘다.

## 2. 이 솔루션의 코딩 규칙

현재 코드에는 사용자의 코드와 Claude·Codex의 리팩토링·추가 코드가 섞여 있고, 2026-11-15 첫 커밋 이전의 Git 이력이 없어 파일별 작성자를 모두 확인할 수는 없다. 아래에서 "예외"로 적은 표기는 기존 코드를 존중해 남겨 둔 것이며, 새 코드의 기준으로 삼지 않는다.

### 프로젝트와 파일

- 솔루션 `MultiplayerGame.sln`에는 `Core`(DLL)와 `ServerApp`(EXE)만 있다. `Tests/*.vcxproj`는 솔루션에 포함되지 않은 별도 시험 프로젝트이며, Core의 CPP를 `CORE_EXPORTS`로 직접 컴파일한다.
- 헤더는 `Public/`, 구현은 `Private/`, 프로젝트 파일과 진입점은 `Default/`에 둔다. `U`로 시작하는 클래스 파일은 클래스 이름과 같다(`UListener.h`, `UListener.cpp`). 그 밖의 파일은 기능 이름을 쓴다(`CustomMemory.h`, `Packet.h`, `ServerApp.h`, `IDGenerator.h`).
- Core·Server는 `stdcpplatest`와 `/utf-8`로 빌드한다. `Tests`의 프로젝트는 `stdcpp20`으로 Core 소스를 컴파일하므로, Core에 C++20보다 새로운 기능을 쓰면 계약 시험 빌드가 깨질 수 있다.
- 소스는 BOM 없는 UTF-8이며, 주석은 한국어로 쓴다.
- 헤더는 `#pragma once`로 시작한다. 다른 Core 클래스는 가능하면 전방 선언하고, 구현 파일에서 include한다.
- Core의 CPP는 대부분 `#include "CoreDefine.h"`를 첫 줄에 두고, 그다음 자기 헤더와 필요한 Core 헤더, 마지막으로 외부·표준 헤더를 둔다(예외: `UJobQueue.cpp`). 표준·Boost.Asio 공통 헤더는 이미 `CoreTypes.h`, `CoreStlTypes.h`가 포함하므로 중복 추가 전에 확인한다.
- Server에 공개하는 Core 클래스에는 `CORE_DLL`을 붙인다. 메모리 풀(`UMemoryPool`, `UMemoryAdiminstor` 등)과 계측 내부 클래스에는 붙어 있지 않다.

### 네임스페이스

- `SPACE_H(Core)` … `SPACE_T`로 감싼다. 하위 영역은 `SPACE_H(Alloc)`, `SPACE_H(Metrics)`처럼 중첩한다. Server 코드는 `SPACE_H(Server)`를 사용한다.
- 파일 안에서만 쓰는 보조 함수는 해당 네임스페이스 안의 익명 `namespace {}`에 둔다.
- 기존 `ServerApp.h`에는 `using namespace Core;`가 있다. 새 헤더에는 같은 선언을 추가하지 않고 `Core::`를 명시한다.

### 이름

| 대상 | 규칙 | 예 |
|---|---|---|
| 클래스 | `U` 접두사 + PascalCase | `UListener`, `ULogger`, `UServerApp` |
| 구조체 | `F` 접두사 | `FLogRecord`, `FNetworkWorkerData`, `FHandlers` |
| 열거형 | `E` 접두사, `enum class`. 기반 타입(`uint8` 등)을 주는 경우가 많다 | `enum class ELogLevel : uint8` |
| 인터페이스 | `I` 접두사 + `abstract` | `IRuntimeMetrics` |
| 전역 접근 | `G` 접두사 | `GCoreGlobal`, `GMemory`, `GThreadMemoryCache` |
| 함수 | PascalCase, 동사로 시작 | `Start`, `RequestStop`, `TryLog`, `GetLocalEndpoint` |
| class 멤버 변수 | `_` + PascalCase | `_Acceptor`, `_Running`, `_CoreInstance` |
| struct 필드 | 접두사 없는 PascalCase | `Level`, `OnFailure`, `ChannelID` |
| 매개변수·지역 변수 | camelCase | `coreInstance`, `onAccepted`, `generation` |
| 네임스페이스 상수 | `STATIC_CONSTEXPR` + 대문자와 `_` | `LOG_MAX_BATCH_RECORDS`, `PACKET_BUFFER_SIZE` |
| 클래스 안 상수 | `static constexpr` 또는 `STATIC_CONSTEXPR` + PascalCase | `ChannelBudget`, `PoolCount` |
| 타입 별칭 | 대문자(필요하면 `_`) | `SHPTR`, `CONQUEUE`, `LOGBATCH_CONTAINER` |

- 함수 이름의 의미를 맞춘다. `Try…`는 실패를 반환값으로 알리는 시도, `Request…`는 기다리지 않는 요청, `On…`은 완료 핸들러, `Is…`/`Has…`는 상태 조회, `Run…Worker`는 스레드 진입 함수다.
- `Get…`이 참조를 반환하면 반환한 참조를 쓰는 동안 소유 객체가 살아 있어야 한다는 조건을 주석으로 적는다.
- 예외의 예:
  - 이름: `_bRunning`, `_idIssuance`, `_allocSize`, `FPacketHeader`의 `type`·`size`, `UThreadManager`의 `Work`·`Data`·`CurrentThreadID`·`Thread`, `UNetworkManager::RunWorker(void* Data)`, 메모리 풀 코드의 `pMemoryPool`·`mHeader`·`eMemoryPoolMetricIndex`.
  - 접두사: 이름은 `F`이지만 class인 `FWorkerLogChannel`, `U`가 없는 클래스 템플릿 `CustomString`, 배열 색인을 위해 `enum class`가 아닌 `EMemoryPoolMetricIndex`, 기반 타입이 없는 `EStopState`·`EResult`.
  - 별칭: 클래스 안의 `F` 접두사 별칭 `FOnFailure`·`FNotify`, 문자열 타입 `FString`·`FWString`.
  - 형태: `final`이 아닌 `UIDGenerator`·`UAllocator`, `explicit`이 아닌 `UObject` 생성자, `ATOMIC_BOOL` 대신 `ATOMIC<bool>`을 쓴 `ServerApp.h`.

### 타입과 메모리

- 정수는 `int8`~`uint64`, 바이트는 `uint8`, 바이트 버퍼는 `BUFFER`(`VECTOR<uint8>`)를 쓴다. 문자열 보기는 `STRINGVIEW`, 시각은 `TIMEPOINT_STEADY_CLOCK`·`TIMEPOINT_SYSTEM_CLOCK`, 원자 변수는 `ATOMIC<T>`·`ATOMIC_BOOL`·`ATOMIC_UINT64`를 쓴다.
- 예외 전달에는 `EXPECTION_PTR`, 표준 시스템 오류에는 `ERROR_CODE`, Boost 오류에는 `BOOST_ERROR_CODE`를 쓴다.
- `VECTOR`, `LIST`, `QUEUE`, `MAP`, `CONQUEUE` 등은 메모리 풀 할당자를 쓴다. 메모리 풀과 런타임 계측 내부는 풀이 자기 자신을 호출하지 않도록 `std::`, `concurrency::` 컨테이너를 직접 쓴다.
- 타입 별칭은 여러 곳에서 반복하거나 공통 의미가 있는 타입에만 둔다. 한 클래스에서만 쓰는 콜백은 `FUNC<void(EXPECTION_PTR)>`처럼 선언 위치에서 인자와 반환형이 보이게 쓴다. 2026-11-12 별칭 정리 때 `LOG_RECORD_HANDLER` 등을, 2026-11-14 ULogger 정리 때 멤버 하나에만 쓰던 `LOGQUEUE`를 이 기준으로 없앴다. 이후 AI가 `UListener`에 추가한 `FOnFailure`는 이 기준에 맞는지 다시 검토할 대상이다.
- Core 객체는 `MakeShared<T>()`로 만들고, 생성자는 `explicit T(SHPTR<UCoreInstance> coreInstance)` 형태로 부모에 전달한다. 자기 참조는 `ThisShared<T>()`, 비동기 람다에 붙잡을 때는 순환 참조를 피하려고 `ThisWeak<T>()`를 쓴다.
- 프로세스 전체에 하나만 있는 기반 객체(`UCoreGlobal`)는 `DECLARE_INSTANCE`/`IMPLEMENT_INSTANCE` 또는 `DECLARE_SHARED_INSTANCE`/`IMPLEMENT_SHARED_INSTANCE`로 만든다. 매니저는 싱글턴으로 만들지 않고 `UCoreInstance`가 소유한다.

### 클래스 선언

- 일반 클래스는 `final`을 쓴다. 기존 상속용 기반 클래스들에는 MSVC `abstract` 표기가 있고, `UListener`는 표준 순수 가상 함수로 추상형을 표현하며 `protected` 생성자를 쓴다. 파생 클래스의 소멸자는 `virtual ~UName() override;`로 선언한다.
- 복사와 이동을 막을 때는 `NO_COPY(UName)`을 쓴다.
- 헤더 안 순서는 `public:` 공개 타입·별칭 → 생성자 → 소멸자 → `NO_COPY` → 공개 함수, `private:` 내부 타입과 내부 함수 → 상수 → 멤버 변수다.
- 멤버 초기값은 선언에서 중괄호로 준다(`bool _Running{ false };`). 생성자 초기화 목록도 `UObject{ coreInstance }`처럼 중괄호를 쓴다(예외: `FPacketHeader`의 `= 0`, `FLogRecord` 생성자의 괄호 초기화).
- 헤더에는 짧은 조회 함수와 템플릿만 정의하고, 나머지는 CPP에 정의한다.

### 함수 계약과 오류 처리

- 공개 함수 위에 한국어 주석으로 **호출 스레드**(제어 스레드·워커), **호출 시점**(Start 전, 종료 후), **수명 조건**, **반환값의 의미**(예: "true는 큐 접수 성공이며 출력 성공이 아니다"), **예외 전달 여부**를 적는다. 구현 주석은 동작보다 이유를 설명한다.
- 성공 여부는 `bool`로 반환한다. 시스템 오류는 `BOOST_ERROR_CODE& error` 같은 출력 매개변수로 돌려주고, 함수 안에서 값을 바꾸는 참조 매개변수에는 `REF_OUT`을 붙일 수 있다.
- 실패 보고·알림·종료 요청 함수는 대부분 `noexcept`다(`RequestStop`, `ReportFailure`, `UListener::Stop`, `UNetworkManager::Shutdown`). `UServerApp::Stop`, `UThreadManager::JoinAll`은 아니다.
- 네트워크 워커의 예외는 `UNetworkManager::RunWorker`가 `EXPECTION_PTR` 콜백으로 전달한다. `UThreadManager`에 등록하는 함수는 오류를 자기 경로로 전달하고 예외를 밖으로 내보내지 않는다. 내보내면 `UThreadManager`가 각 스레드에 설정한 종료 처리기가 예외 내용을 stderr에 남기고 `abort`한다. MSVC의 `std::set_terminate`는 스레드마다 따로 적용되므로 스레드 안에서 설정한다. (2026-11-16 사용자 선택: "catch를 제거하지만 예외는 알 수 있게 한다")
- 조건 실패 시 바로 반환한다(`RETURN_INVALID_COND`, `if (...) return false;`). 메모리 풀 코드(`CustomMemory.h`)와 `IMPLEMENT_INSTANCE`는 `nullptr == ptr`, `0 != value`처럼 상수를 왼쪽에 두고 비교하지만, 다른 코드에는 `!ptr`, `if (_Instance)` 형태도 섞여 있다.

### 동기화 구현

- 1절의 잠금 규칙을 따른다. 잠금이 필요한 private 함수에는 "`_Mutex`를 소유한 상태에서 호출한다"를 주석으로 적는다.
- 사용자 콜백, 알림, 입출력은 가능하면 잠금 밖에서 실행한다. 공유 객체를 잠금 안에서 복사하고 잠금을 푼 뒤 호출한다(`UListener::OnAccept`, `FWorkerLogChannel::Notify`).
- 원자 변수는 필요한 경우 `memory_order`를 명시한다.

### 서식

- 함수 정의의 여는 중괄호는 다음 줄에 둔다. 클래스 선언과 제어문의 중괄호 위치는 해당 파일의 기존 형식을 따른다.
- 들여쓰기는 파일마다 탭 또는 공백 4칸이다. `UListener`, `USession`, `ULogger.cpp`, `FWorkerLogChannel`, `UCoreInstance`, `UJobQueue.h`, `Tests`는 공백 4칸이고, 나머지 대부분은 탭이다. `CoreMacro.h`는 둘이 섞여 있다.
- 줄바꿈도 파일마다 LF 또는 CRLF 한 가지를 쓴다. 2026-11-13 기준 Core·Server 46개 파일 중 LF 25개, CRLF 21개이며, 한 파일 안에서 섞인 경우는 없다. 헤더와 CPP의 형식이 다를 수도 있다(`UThreadManager.h`는 CRLF, `.cpp`는 LF).
- 수정할 때는 파일의 기존 들여쓰기와 줄바꿈을 유지하고, 파일 전체를 다시 정렬하지 않는다. 새 파일은 짝이 되는 파일이나 같은 폴더의 파일 형식을 따른다.
- 한 헤더에 여러 클래스나 묶음이 있으면 `/*`, `----`, 이름, `----`, `*/` 형태의 구분 주석 블록으로 나눈다(`CustomMemory.h`, `CoreStlTypes.h`).

### GraduateProject와의 차이

- `GraduateProject` 서버는 `AServerService`, `CServerAdiminstor`처럼 `A`·`C` 접두사와 `m_` 멤버 표기를 쓴다. 현재 `MultiServer`의 `U`·`F`·`_` 표기와 다르므로 과거 이름을 일괄 이식하지 않는다.
- `GraduateProject`의 `AServerService`는 생성 시 수락기를 준비하고, `CServerAdiminstor::Start()`에서 첫 `Connect()`를 건 뒤 상위 `Start()`가 네트워크 워커를 등록한다. 이는 과거 코드에서 확인한 순서이며, 현재 리스너의 종료·재시작 동시성까지 보장한다는 뜻은 아니다.
- 현재 코드에는 Claude의 리팩토링과 이후 수정이 함께 있다. Git 전후 이력(2026-11-15 첫 커밋 이후만 있다)이나 명시적인 규칙 원문이 없는 경우 작성자나 사용자 승인 여부를 추정하지 말고, 확인된 코드 관례와 직접 받은 지시를 구분한다.

## 3. AI 작업 규칙

Claude·Codex 등 AI가 이 솔루션에서 코드를 다룰 때 지킨다.

### 작성 전

- 이 문서, 프로젝트 기억·RAG에 남은 사용자 지시, `CoreTypes.h`·`CoreStlTypes.h`·`CoreMacro.h`, 수정할 파일과 그 호출 경로를 먼저 읽는다. 이전 대화의 지시를 놓치지 않도록 관련 키워드로 다시 조회한다.
- 무엇을 바꿀지 사용자 지시와 현재 코드의 근거로 설명할 수 있어야 한다. 근거가 없는 설계 결정은 구현하지 않고 선택지로 먼저 제시한다.
- 사용자가 직접 작성 중인 Core 코드는 요청 없이 고치지 않는다. 분석이나 확인만 요청하면 분석만 하고 소스는 바꾸지 않는다.

### 작성 중

- 요청받은 클래스와 함수만 최소 범위로 작성한다. 요청에 없는 재시작 지원, 재시도, 세대 번호, 상태 값, 새 별칭, 새 클래스, 방어용 잠금을 "안전을 위해" 추가하지 않는다. 필요하다고 판단하면 이유와 비용을 설명하고 사용자가 선택하게 한다.
- 잠금을 넣기 전에 상태마다 **누가(어느 스레드) 언제 읽고 쓰는지**, 객체가 언제 다른 스레드에 공개되는지 적어 본다. 객체가 다른 스레드에 공개되기 전이고 제어 스레드만 접근한다고 보장된 초기화에는 잠금을 걸지 않는다. 이미 완료 핸들러나 워커가 접근할 수 있는 상태라면 초기화 함수라도 동기화를 검토한다. 자주 실행되는 경로(로그 한 건, 접속 한 건)에서 잠금을 몇 번 잡는지 세어 설명한다.
- 잠금 범위는 공유 상태를 읽고 바꾸는 구간으로 줄인다. 잠금 안에서 콜백, 입출력, 할당, 시스템 호출을 실행해야 하면 그 이유를 주석과 보고에 남긴다.
- 작성 후 직접 쓴 코드에서 `size_t`, `std::size_t`, `std::mutex`, `std::thread::id`, `boost::asio::`, `std::shared_ptr`, `std::unique_ptr`, `std::function` 등 프로젝트에 별칭이 있는 타입의 직접 표기를 검색해 확인한다. 크기·개수 등의 `size_t`는 `uint64`를 사용하고, 다른 표기도 기존 별칭을 사용한다. OS·표준 라이브러리 경계의 비트 폭·부호·범위 검사는 보존하며, 할당자가 다른 타입과 별칭이 없는 `std::unique_lock<SHARED_MUTEX>`는 이름만 보고 바꾸지 않는다.
- 헤더의 계약 주석은 코드와 함께 고친다. 동작을 바꿨는데 주석이 옛 동작을 설명하면 안 된다.
- 파일을 수정한 뒤 들여쓰기 문자와 CRLF 개수를 원본과 Python으로 비교한다. Git Bash의 `grep -c $'\r$'` 결과는 믿지 않는다.

### 검증

- 빌드와 시험은 `%TEMP%` 아래 분리 복사본에서 VS MSBuild로 x64 Debug·Release를 실행한다. Boost 경로는 `-p:BoostIncludeDir=<저장소>\ThirdParty\boost_1_92_0`으로 넘긴다. 실제 `Server/Bin`은 덮어쓰지 않는다.
- 변경 범위에 해당하는 계약 시험 `MemoryContracts`, `LoggerContracts`, `ListenerContracts`, `SessionContracts`(세션·컴포넌트·SafeGuard·다중 방 계약 포함) 4종을 다시 실행한다. 시험 프로젝트가 경로 옵션을 받지 않으면 복사본에서만 보정한다. `MemoryWalkthrough`, `ThreadSchedulerTests`는 없는 파일(`ACoreBase.cpp`, `GlobalFunc.cpp`, `UJobScheduler.cpp`)을 참조하는 오래된 프로젝트라 회귀 시험으로 쓰지 않는다.
- 빌드 성공을 동시성 정확성이나 성능 증명으로 보고하지 않는다. 처리량·대기 시간을 측정하지 않았으면 "최적화됐다", "잠금이 적다"고 말하지 않는다.
- 작업이 끝나면 이번 작업에서 만든 복사본과 임시 시험 파일, 이번 빌드가 만든 `%TEMP%\_CL_*`만 지운다. 다른 빌드의 임시 파일은 건드리지 않는다.

### 기록과 보고

- 변경 내용과 검증 결과는 `로그_설계_자료/로그_설계_기록.pdf` 끝에 한 쪽을 추가해 남긴다. 기존 쪽은 고치지 않는다. 이 기록 방식은 사용자가 결과를 추적하는 기존 관례다.
- 보고할 때는 사용자의 선택, AI의 제안, 현재 코드의 동작, 시험하지 않은 범위를 구분한다. 지시를 놓쳤거나 잘못 판단했으면 숨기지 않고 무엇을 놓쳤는지 밝힌다.
- Windows 명령은 CMD 또는 Git Bash로 실행한다. Codex·Claude 등 CMD/Git Bash를 선택할 수 있는 실행기는 PowerShell을 직접·간접 모두 사용하지 않는다(사용자 공용 지시). 단, 내장 셸이 PowerShell로 고정된 실행기(Antigravity/Gemini)에 한해 PowerShell 고유 문법·`.ps1` 없이 `cmd /c`·`python`·`git` 호출 용도로만 예외 허용한다(2026-11-20 사용자 정정).

### Git

- 이 폴더는 Git 저장소다(2026-11-15 도입, 첫 커밋은 도입 시점의 상태). 기준 저장은 사용자가 `D:\__job\tools\ChangeViewer`의 버튼으로 하고, AI는 사용자가 요청할 때만 커밋한다.
- 작업 폴더를 되돌리거나 지우는 Git 명령(`checkout -- <파일>`, `restore`, `reset --hard`, `stash`, `clean`, `rm`)은 쓰지 않는다. `clean -fdx`는 Git이 제외한 PDF·Bin·ThirdParty까지 지운다. (2026-11-15 사용자 선택: "되돌리기·삭제만 금지")
