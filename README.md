# MultiServer — C++20 Boost.Asio 멀티플레이어 채팅 서버 & 런타임 관측 아키텍처

> **기준일**: 2026-11-22 · **언어/스택**: C++20/Latest (MSVC v145, x64) · Boost.Asio 1.92.0 · Protocol Buffers v33.6 · Custom Memory Pool · Async `ULogger` · `CVTRACE1` + `.cvlog` 이중 관측 파이프라인
>
> 본 프로젝트는 **Core(DLL) – Server(EXE) – Protocol(Protobuf) – 별도 C# WinForms Client([../Client](../Client))** 4계층으로 분리된 고동시성 다중 방 채팅 서버와, AI 협업 개발의 변경·프로토콜·런타임 호출을 직접 검증하기 위한 관측 도구(**ChangeViewer**) 연동 구조를 함께 구현·실측한 프로젝트입니다.

---

## 1. 아키텍처 개요

```text
┌────────────────────────────────────────────────────────────────────────────┐
│  ChatClient (.NET 10 WinForms, ../Client)  /  Python E2E Verifier (tools)  │
│  [4B LE Header: uint16 type | uint16 size] + [Protobuf Body <= 1,020 B]    │
└───────────────────────────────────┬────────────────────────────────────────┘
                                    │ TCP (127.0.0.1:<port>)
┌───────────────────────────────────▼────────────────────────────────────────┐
│  Server (ServerApp.exe)                                                    │
│  UServerApp ─ UServerListener ─ UServerSession (로그인 전 핸드셰이크 전용) │
│  UPlayer (UPawn 컴포넌트 소유, 도메인 패킷·방 이동·전역 닉네임 RAII 관리)  │
│  UGameRoom × 4 (로비/자유/전략/거래, 독립 Strand · 내부 mutex 0 · 25ms 병합)│
├────────────────────────────────────────────────────────────────────────────┤
│  Core (Core.dll — Protobuf 비의존 · dynamic_cast 0건)                      │
│  UCoreInstance ─ UThreadManager / UNetworkManager / ULogger / URoomManager │
│  UComponent · UPawn(CONUNORMAP) · USession(Shared Frame·1MiB Cap·유휴타이머)│
│  FPacketView(NO_COPY) + FPacketPayload(Detach 불변) · CustomMemory TLS 풀  │
│  RuntimeTrace (Off / FlightRecorder / AsyncStream → .cvtrace + .cvlog)     │
└────────────────────────────────────────────────────────────────────────────┘
```

### 계층 경계 및 책임 분리
- **Core (`Core/Public`, `Core/Private`)**: 소켓 생명주기(`UListener`, `USession`), 스레드/네트워크/로그/방 매니저(`UCoreInstance` 소유), 게임 사물 기반 계층(`UObject → UActor → UPawn → UCharacter`), 크기별 메모리 풀(`CustomMemory`), 런타임 계측(`RuntimeTrace`). **Protobuf에 일절 의존하지 않습니다.**
- **Server (`Server/Public`, `Server/Private`)**: 패킷 역직렬화·검증(`UServerSession::HandlePacket<TPacket>`), 채팅 도메인 규칙(`UGameRoom`, `UPlayer`, `ChatProtocolLimits.h`), 실행 진입점(`ServerApp`, `Main.cpp`). 소켓 API를 직접 호출하지 않습니다.
- **Protocol (`Protocol/Schema`)**: 방향별 패킷 번호(`packet_type.proto`)와 도메인별 메시지(`login.proto`, `chat.proto`, `room.proto`) 단일 출처. `Build_Protocol.bat`으로 C++(`GameProtocol.lib`)과 C# 코드를 동시 생성하고 SHA-256 동기화를 검사합니다.

---

## 2. 핵심 설계 의사결정 5가지

1. **방 내부 무잠금 Strand 직렬화 (`Core::URoom` / `Server::UGameRoom`)**
   - 방별 `ASIO::strand`에서 액터 컨테이너(`_ActorContainer`, O(1) swap-and-pop 인덱스 맵)와 닉네임 인덱스(`_NicknameIndex`)를 전담 처리해 **방 내부 `mutex`를 0개로 제거**했습니다.
   - `Core::URoomManager`는 런타임 방 삭제를 허용하지 않는 Append-Only 정책으로 단일 `CONUNORMAP` 조회/등록을 무잠금으로 수행합니다.
2. **브로드캐스트 O(N) → O(1) 공유 프레임 + Scatter-Gather + 25ms 인원 병합**
   - `USession::MakePacketBuffer`로 `[4B 헤더 + 직렬화 본문]` 불변 버퍼를 1회만 생성하고 `SendShared` / `BroadcastSharedOnStrand`로 N명 세션에 공유합니다.
   - `USession`은 대기 큐에서 최대 **64프레임**을 Scatter-Gather 배치 `async_write`로 묶어 시스템 콜 횟수를 줄이고, 느린 클라이언트의 송신 큐가 **1 MiB(1,048,576 B)**를 초과하면 Warning 로그를 남기고 즉시 연결을 차단합니다(백프레셔 보호).
   - 1,000명 동시 입장/퇴장 폭주 시 `SC_ROOM_USER_COUNT`를 **25 ms Coalescing**으로 병합해 불필요한 폭주 패킷의 **98.54%**를 제거했습니다.
3. **공용 `HandlePacket<TPacket>` & 명확한 오류/경고 정책**
   - 패킷마다 중복 핸들러를 늘리지 않고 `UServerSession::HandlePacket<TPacket>`으로 역직렬화 경로를 단일화했습니다.
   - 규칙 위반 채팅(빈 문자열·200자 초과·비정상 UTF-8)은 사유와 바이트 수를 Warning으로 기록하고 연결을 유지하며, 미정의 패킷 번호·Protobuf 파싱 실패·헤더 1,020 B 초과는 Warning 후 해당 연결만 즉시 닫습니다.
4. **이중 관측 파이프라인 (`CVTRACE1` 이진 `.cvtrace` + JSONL `.cvlog`)**
   - 네트워크 핫패스에는 256 B 고정 이진 슬롯(`Off` / `FlightRecorder` 16,384슬롯 / `AsyncStream` 4,096슬롯)만 기록하고, 운영 로그(`ULogger`)는 전용 워커 스레드 배치 콜백에서 동반 `.cvlog`를 기록해 `traceSeq`로 1:1 연결합니다.
   - `/DCORE_ENABLE_RUNTIME_TRACE=0` 컴파일 스위치로 계측 코드를 완전히 제거(Zero-Cost)할 수 있습니다.
5. **엄격한 코딩 규율과 서드파티 경고 격리**
   - `AGENTS.md` 규칙(`friend` 전면 금지, 일반 `std::mutex` 금지 → `SHARED_MUTEX` + 읽기/쓰기 잠금 구분, `size_t` 대신 `uint64` 및 프로젝트 별칭 일관 사용)을 정적 검사로 강제하고, `Protocol/ServerProtocol.props`에 `<ExternalIncludePath>` + `/external:W0`를 적용해 우리 코드 `/W3` 검사를 유지한 채 **Debug/Release x64 경고 0건**을 달성했습니다.

---

## 3. 정량 실측 및 검증 결과 (x64 Release 기준)

> 모든 수치는 [로그_설계_자료/로그_설계_기록.pdf](로그_설계_자료/로그_설계_기록.pdf) 478~491쪽에 기록된 실제 측정값입니다.

### ① Phase 2 최적화 및 1,000명 규모 벤치마크 (480쪽)
| 측정 항목 | 최적화 전 (Before) | 최적화 후 (After) | 개선 폭 |
|---|---:|---:|---:|
| RuntimeTrace 마이크로벤치 (8스레드, 200,000건) | 1,379.60 ms (`0.14 Mev/s`) | Off `1.77 ms` (`113.22 M/s`)<br>Flight `28.66 ms` (`6.98 M/s`)<br>Async `35.11 ms` (`5.70 M/s`) | Off **779×**<br>Flight **48.1×**<br>Async **39.3×** |
| 1,000명 동시 로그인 폭주 (`TRACE_OFF`) | 1,738.97 ms (`CountPkts 358,919`) | **344.45 ms** (`CountPkts 5,247`) | **5.05×** (`-80.2%`)<br>패킷 **-98.54%** |
| 1,000명 방 채팅 30×1,000 = 30,000건 전달 (`TRACE_OFF`) | 747.28 ms (`40.15 Kmsg/s`, Err 0) | **62.42 ms** (`480.59 Kmsg/s`, Err 0) | **11.97×** (`+1,097%`) |
| 1,000명 + `FLIGHT` 트레이스 상시 ON | 미지원 | 로그인 `386.85 ms` (`5,871 pkt`)<br>채팅 `120.62 ms` (`248.71 Kmsg/s`) | 최적화 전 OFF 대비 채팅 **6.19×** |
| 1,000명 + `ASYNC` 트레이스 상시 ON | 동기 기록 `3,103,827건` (`794.6 MB`) 크래시 (`orderlyEnd=F`) | 로그인 `355.09 ms` / 채팅 `115.36 ms` (`260.05 Kmsg/s`), `Err=0`, `orderlyEnd=True` | 로그인 **4.90×**<br>채팅 **6.48×** |

### ② 통합 안정성 · 백프레셔 · 크로스 언어 E2E 검증 (482·488~491쪽)
- **다중 부하 + 중도 이탈 (489쪽)**: 120명 동시 접속(정상 100 · 중복 거절 10 · 규칙 위반 거절 10) + 40명 동시 이탈 + 잔류 60명 대상 채팅 200건 브로드캐스트 무손실 수신(`850.3 ms`).
- **경고 정책 및 1 MiB 백프레셔 차단 (489쪽)**: 빈 채팅/201한글자/220영문자 경고 후 연결 유지, 깨진 Protobuf·미정의 999·헤더 1,021 B 초과 경고 후 즉시 차단, 비수신 `SlowReader` 1 MiB 송신 큐 초과 강제 차단(`.cvlog` 실측).
- **C++ 서버 ↔ C# WinForms 클라이언트 E2E (482·490·491쪽)**: 한글 닉네임·중복/규칙 거절·3바이트 분할/2패킷 병합 프레이밍·25명 20명 단위 페이징·양방향 채팅·`CVTRACE1`/`.cvlog` 실행 추적 모두 PASS.
- **빌드 & Core 계약 시험 (488~491쪽)**: `Debug|x64` · `Release|x64` 모두 **오류 0 / 경고 0**, `ListenerContracts`(7/7) · `LoggerContracts`(3/3) · `MemoryContracts`(7/7) 전항목 PASS.

---

## 4. 프로토콜 요약 (`Protocol/Schema`)

TCP 프레임 형식: `[uint16 type (LE)] [uint16 size (LE)] [Protobuf Body (0..1,020 B)]` (총 최대 1,024 B)

| 방향 | 패킷 Enum | 번호 | 메시지 | 설명 / 한도 |
|---|---|---:|---|---|
| C → S | `CS_LOGIN` | 1 | `CSLogin` | 닉네임 2~12자(완성형 한글·영문·숫자), 서버 전역 중복 시 거절 |
| C → S | `CS_CHAT` | 2 | `CSChat` | 채팅 본문 1~200자 UTF-8 (초과 시 Warning, 연결 유지) |
| C → S | `CS_ROOM_USERS` | 3 | `CSRoomUsers` | `offset` 기준 현재 방 참가자 목록 요청 |
| C → S | `CS_ROOM_LIST` | 4 | `CSRoomList` | 고정 4채널 목록·인원 수 조회 |
| C → S | `CS_JOIN_ROOM` | 5 | `CSJoinRoom` | 지정 방으로 크로스-스트랜드 이동 요청 |
| C → S | `CS_PING` | 6 | `CSPing` | 유휴 연결 유지 및 RTT 측정 하트비트 |
| S → C | `SC_LOGIN_RESULT` | 1 | `SCLoginResult` | `ELoginResult` + 입장 방 ID + 현재 인원 수 |
| S → C | `SC_CHAT` | 2 | `SCChat` | 발신자 닉네임 + 채팅 본문 (현재 방 전체 브로드캐스트) |
| S → C | `SC_ROOM_USER_COUNT` | 3 | `SCRoomUserCount` | 25 ms 병합 인원 수 알림 (방당 최대 1,000명) |
| S → C | `SC_ROOM_USERS` | 4 | `SCRoomUsers` | 최대 20명 닉네임 목록 + `total` |
| S → C | `SC_ROOM_LIST` | 5 | `SCRoomList` | 고정 4채널(`로비`/`자유`/`전략`/`거래`) 요약 목록 |
| S → C | `SC_JOIN_ROOM_RESULT` | 6 | `SCJoinRoomResult` | `EJoinRoomResult` + 최종 소속 방 ID + 인원 수 |
| S → C | `SC_PONG` | 7 | `SCPong` | `CS_PING` 타임스탬프 즉시 에코 |

---

## 5. 빌드 · 실행 · 원클릭 재현 검증

### 사전 준비
- Visual Studio 2022/2026 C++ 도구(`v145`, x64), Python 3.10+, Boost 1.92.0 헤더(`ThirdParty/boost_1_92_0` 또는 `-p:BoostIncludeDir=...`).

### 1) 프로토콜 생성 및 서버 빌드
```bat
Build_Protocol.bat Release
Update_Core_Server.bat Release
```

### 2) 서버 실행
```bat
Server\Bin\Release\ServerApp.exe 4 7777 --trace=stream
```
- 인자: `ServerApp.exe <네트워크 워커 수> <포트> [--trace=stream|flight|off]` (`127.0.0.1:<포트>` 바인딩, `Enter` 키로 정상 종료)
- 트레이스 모드 생략 시 기본값은 `--trace=stream`이며, 성능 측정 전용 실행 시 `--trace=off`를 지정합니다.

### 3) 원클릭 E2E · 부하 · 백프레셔 재현 검증
```bat
python tools\run_e2e_verify.py
```
- 외부 Python 패키지 없이 표준 라이브러리만으로 `ServerApp.exe`를 임시 포트에 띄워 **로그인/중복/규칙 거절 · TCP 1B 분할/2패킷 병합 · 25명 페이징 · 경고/차단 5종 · 100명 동시 부하/이탈 · 1 MiB SlowReader 백프레셔 · 정상 종료 및 `.cvtrace`/`.cvlog` 검증**을 한 번에 재현합니다.

### 4) Core 계약 단위 시험
```bat
msbuild Tests\MemoryContracts.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
Tests\.build\Debug\bin\MemoryContracts.exe
msbuild Tests\LoggerContracts.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
Tests\.build\LoggerContracts\Debug\bin\LoggerContracts.exe
msbuild Tests\ListenerContracts.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64
Tests\.build\ListenerContracts\Debug\bin\ListenerContracts.exe
```
> **참고 (보존/예약 항목)**: `Tests/` 내 `MemoryWalkthrough`, `ThreadSchedulerTests`, `SessionContracts`는 초기 구버전 API를 참조하는 아카이브 프로젝트로 현재 회귀 시험 대상이 아닙니다. `Core/Public/UJobQueue.h`는 추후 게임 루프 확장용으로 보존된 예약 클래스입니다.

---

## 6. 프로젝트 폴더 구조

- `Core/` — 공통 네트워크·메모리 풀·비동기 로거·액터/방 기반·런타임 트레이스 DLL
- `Server/` — 채팅 서버 애플리케이션(`ServerApp`, `UServerListener`, `UServerSession`, `UGameRoom`, `UPlayer`)
- `Protocol/` — `.proto` 스키마, `Build_Protocol.bat` 생성 파이프라인, `ServerProtocol.props`(`/external:W0`)
- `Tests/` — Core 계약 회귀 시험(`MemoryContracts`, `LoggerContracts`, `ListenerContracts`)
- `tools/` — 프로토콜 빌드/검증 스크립트 및 원클릭 E2E 재현기(`run_e2e_verify.py`)
- `로그_설계_자료/` — 설계·의사결정·실측 기록(`로그_설계_기록.pdf`, 491쪽, 대용량으로 Git 제외)
