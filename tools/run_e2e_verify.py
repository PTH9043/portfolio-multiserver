#!/usr/bin/env python3
"""MultiServer 원클릭 E2E · 다중 부하 · 경고 정책 · 1 MiB 백프레셔 재현 검증기.

외부 Python 패키지 없이 표준 라이브러리만으로 ServerApp.exe를 임시 포트에 실행하고
아래 6개 핵심 시나리오를 순서대로 검증합니다.
  1. 로그인 정책 (한글·영문 성공 / 중복 거절 / 규칙 위반 거절 / 중복 로그인 거절)
  2. 양방향 채팅 + TCP 1바이트 분할 전송 + 2패킷 병합 전송 프레이밍 검증
  3. 25명 동시 접속 및 20명 단위 페이징(offset 0 / 20 / 25) + 퇴장 인원 갱신
  4. 경고·차단 정책 5종 (빈 채팅·201자 초과 유지 / 깨진 Protobuf·미정의 999·헤더 1021B 차단)
  5. 다중 동시 부하 (100명 로그인 + 30명 중도 이탈 + 100건 채팅 무손실 수신)
  6. SlowReader 1 MiB 송신 큐 초과 백프레셔 차단 + 정상 종료(rc=0) 및 .cvtrace/.cvlog 확인
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import os
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

# 패킷 번호 (Protocol/Schema/packet_type.proto)
CS_LOGIN = 1
CS_CHAT = 2
CS_ROOM_USERS = 3

SC_LOGIN_RESULT = 1
SC_CHAT = 2
SC_ROOM_USER_COUNT = 3
SC_ROOM_USERS = 4

# ELoginResult (Protocol/Schema/login.proto)
LOGIN_SUCCESS = 1
LOGIN_INVALID_NICKNAME = 2
LOGIN_DUPLICATE_NICKNAME = 3
LOGIN_ALREADY_LOGGED_IN = 4
LOGIN_ROOM_FULL = 5

PACKET_HEADER_SIZE = 4
PACKET_MAX_BODY_SIZE = 1020


# ---- 최소 Protobuf 와이어 코덱 (외부 의존성 없음) ----

def encode_varint(value: int) -> bytes:
    if value < 0:
        value &= (1 << 64) - 1
    out = bytearray()
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


def decode_varint(data: bytes, pos: int) -> Tuple[int, int]:
    shift = 0
    result = 0
    while True:
        if pos >= len(data):
            raise ValueError("Truncated varint")
        b = data[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7


def encode_string_field(field_no: int, text: str) -> bytes:
    raw = text.encode("utf-8")
    if not raw:
        return b""
    tag = (field_no << 3) | 2
    return encode_varint(tag) + encode_varint(len(raw)) + raw


def encode_uint32_field(field_no: int, value: int) -> bytes:
    if value == 0:
        return b""
    tag = (field_no << 3) | 0
    return encode_varint(tag) + encode_varint(value)


def decode_fields(data: bytes) -> Dict[int, List[object]]:
    fields: Dict[int, List[object]] = {}
    pos = 0
    n = len(data)
    while pos < n:
        tag, pos = decode_varint(data, pos)
        field_no = tag >> 3
        wire_type = tag & 0x07
        if wire_type == 0:
            val, pos = decode_varint(data, pos)
            fields.setdefault(field_no, []).append(val)
        elif wire_type == 2:
            length, pos = decode_varint(data, pos)
            end = pos + length
            if end > n:
                raise ValueError("Truncated length-delimited field")
            fields.setdefault(field_no, []).append(data[pos:end])
            pos = end
        else:
            raise ValueError(f"Unsupported wire type {wire_type}")
    return fields


def frame_packet(packet_type: int, body: bytes) -> bytes:
    return struct.pack("<HH", packet_type, len(body)) + body


def cs_login(nickname: str) -> bytes:
    return frame_packet(CS_LOGIN, encode_string_field(1, nickname))


def cs_chat(text: str) -> bytes:
    return frame_packet(CS_CHAT, encode_string_field(1, text))


def cs_room_users(offset: int) -> bytes:
    return frame_packet(CS_ROOM_USERS, encode_uint32_field(1, offset))


def parse_sc_login_result(body: bytes) -> Tuple[int, int]:
    f = decode_fields(body)
    result = int(f.get(1, [0])[-1])
    user_count = int(f.get(2, [0])[-1])
    return result, user_count


def parse_sc_chat(body: bytes) -> Tuple[str, str]:
    f = decode_fields(body)
    nick = bytes(f.get(1, [b""])[-1]).decode("utf-8")
    text = bytes(f.get(2, [b""])[-1]).decode("utf-8")
    return nick, text


def parse_sc_room_user_count(body: bytes) -> int:
    f = decode_fields(body)
    return int(f.get(1, [0])[-1])


def parse_sc_room_users(body: bytes) -> Tuple[int, int, List[str]]:
    f = decode_fields(body)
    offset = int(f.get(1, [0])[-1])
    total = int(f.get(2, [0])[-1])
    nicks = [bytes(x).decode("utf-8") for x in f.get(3, [])]
    return offset, total, nicks


# ---- 소켓 헬퍼 ----

class TestClient:
    def __init__(self, port: int, timeout: float = 5.0) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(timeout)
        self._buf = bytearray()

    def send_raw(self, data: bytes) -> None:
        self.sock.sendall(data)

    def recv_packet(self, timeout: Optional[float] = None) -> Optional[Tuple[int, bytes]]:
        old_to = self.sock.gettimeout()
        if timeout is not None:
            self.sock.settimeout(timeout)
        try:
            while True:
                if len(self._buf) >= PACKET_HEADER_SIZE:
                    ptype, size = struct.unpack_from("<HH", self._buf, 0)
                    total = PACKET_HEADER_SIZE + size
                    if len(self._buf) >= total:
                        body = bytes(self._buf[PACKET_HEADER_SIZE:total])
                        del self._buf[:total]
                        return ptype, body
                chunk = self.sock.recv(4096)
                if not chunk:
                    return None
                self._buf.extend(chunk)
        finally:
            if timeout is not None:
                self.sock.settimeout(old_to)

    def recv_expect(self, expected_type: int, timeout: float = 5.0) -> bytes:
        deadline = time.monotonic() + timeout
        while True:
            rem = deadline - time.monotonic()
            if rem <= 0:
                raise TimeoutError(f"Timed out waiting for packet type {expected_type}")
            pkt = self.recv_packet(timeout=rem)
            if pkt is None:
                raise ConnectionError(f"Connection closed while waiting for packet type {expected_type}")
            ptype, body = pkt
            if ptype == expected_type:
                return body

    def expect_closed(self, timeout: float = 3.0) -> bool:
        deadline = time.monotonic() + timeout
        while True:
            rem = deadline - time.monotonic()
            if rem <= 0:
                return False
            try:
                pkt = self.recv_packet(timeout=rem)
                if pkt is None:
                    return True
            except (ConnectionResetError, ConnectionAbortedError, OSError):
                return True
            except TimeoutError:
                return False

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def wait_port_ready(port: int, timeout: float = 5.0) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"ServerApp did not listen on 127.0.0.1:{port} within {timeout}s")


# ---- 시나리오 ----

def run_scenarios(port: int, load_clients: int, chat_count: int) -> Dict[str, object]:
    stats: Dict[str, object] = {}

    # 1. 로그인 정책 (성공 / 중복 / 규칙 위반 / 중복 로그인)
    c1 = TestClient(port)
    c1.send_raw(cs_login("테스터01"))
    res, cnt = parse_sc_login_result(c1.recv_expect(SC_LOGIN_RESULT))
    assert res == LOGIN_SUCCESS and cnt == 1, f"Login failed: {res}, {cnt}"

    c_dup = TestClient(port)
    c_dup.send_raw(cs_login("테스터01"))
    res_dup, _ = parse_sc_login_result(c_dup.recv_expect(SC_LOGIN_RESULT))
    assert res_dup == LOGIN_DUPLICATE_NICKNAME, f"Expected duplicate: {res_dup}"
    c_dup.close()

    c_bad = TestClient(port)
    c_bad.send_raw(cs_login("Bad!"))
    res_bad, _ = parse_sc_login_result(c_bad.recv_expect(SC_LOGIN_RESULT))
    assert res_bad == LOGIN_INVALID_NICKNAME, f"Expected invalid nick: {res_bad}"
    c_bad.close()

    c1.send_raw(cs_login("테스터99"))
    res_again, _ = parse_sc_login_result(c1.recv_expect(SC_LOGIN_RESULT))
    assert res_again == LOGIN_ALREADY_LOGGED_IN, f"Expected already logged in: {res_again}"
    print("[PASS] 1. 로그인 정책 (한글 성공 / 중복·규칙위반·재로그인 거절)")

    # 2. 양방향 채팅 + 1바이트 분할 전송 + 2패킷 병합 전송
    c2 = TestClient(port)
    c2.send_raw(cs_login("Player02"))
    res2, cnt2 = parse_sc_login_result(c2.recv_expect(SC_LOGIN_RESULT))
    assert res2 == LOGIN_SUCCESS and cnt2 == 2, f"c2 login failed: {res2}, {cnt2}"

    # 1바이트씩 분할 전송
    split_pkt = cs_chat("분할전송테스트")
    for i in range(len(split_pkt)):
        c1.send_raw(split_pkt[i:i + 1])
    n1, t1 = parse_sc_chat(c2.recv_expect(SC_CHAT))
    assert n1 == "테스터01" and t1 == "분할전송테스트", f"Split chat mismatch: {n1}/{t1}"
    n1_self, t1_self = parse_sc_chat(c1.recv_expect(SC_CHAT))
    assert n1_self == "테스터01" and t1_self == "분할전송테스트"

    # 2패킷 병합 전송
    merged = cs_chat("병합첫째") + cs_chat("병합둘째")
    c2.send_raw(merged)
    got = [parse_sc_chat(c1.recv_expect(SC_CHAT)), parse_sc_chat(c1.recv_expect(SC_CHAT))]
    assert got == [("Player02", "병합첫째"), ("Player02", "병합둘째")], f"Merged mismatch: {got}"
    # c2 측 본인 에코 2건 비우기
    _ = c2.recv_expect(SC_CHAT)
    _ = c2.recv_expect(SC_CHAT)
    print("[PASS] 2. 양방향 채팅 · 1B 분할 전송 · 2패킷 병합 프레이밍")

    # 3. 25명 동시 접속 + 20명 페이징 + 퇴장 인원 갱신
    extra: List[TestClient] = []
    for i in range(3, 26):
        tc = TestClient(port)
        tc.send_raw(cs_login(f"유저{i:02d}"))
        r, _ = parse_sc_login_result(tc.recv_expect(SC_LOGIN_RESULT))
        assert r == LOGIN_SUCCESS, f"Extra user {i} failed: {r}"
        extra.append(tc)

    c1.send_raw(cs_room_users(0))
    off0, tot0, page0 = parse_sc_room_users(c1.recv_expect(SC_ROOM_USERS))
    c1.send_raw(cs_room_users(20))
    off1, tot1, page1 = parse_sc_room_users(c1.recv_expect(SC_ROOM_USERS))
    c1.send_raw(cs_room_users(25))
    off2, tot2, page2 = parse_sc_room_users(c1.recv_expect(SC_ROOM_USERS))
    union_nicks = set(page0) | set(page1)
    assert off0 == 0 and tot0 == 25 and len(page0) == 20
    assert off1 == 20 and tot1 == 25 and len(page1) == 5
    assert off2 == 25 and tot2 == 25 and len(page2) == 0
    assert len(union_nicks) == 25, f"Expected 25 unique users, got {len(union_nicks)}"

    for tc in extra:
        tc.close()
    c2.close()
    # 인원 감소 확인 (25ms coalescing 반영 대기 후 페이징으로 최종 1명 확인)
    deadline = time.monotonic() + 3.0
    final_tot = -1
    while time.monotonic() < deadline:
        c1.send_raw(cs_room_users(0))
        _, final_tot, _ = parse_sc_room_users(c1.recv_expect(SC_ROOM_USERS))
        if final_tot == 1:
            break
        time.sleep(0.05)
    assert final_tot == 1, f"Expected 1 user after leave, got {final_tot}"
    print("[PASS] 3. 25명 접속 · 20명 페이징(20+5+0) · 퇴장 인원 갱신")

    # 4. 경고·차단 정책 5종
    # 4a) 빈 채팅 · 201한글자 초과 -> 경고 후 연결 유지
    c1.send_raw(cs_chat(""))
    c1.send_raw(cs_chat("가" * 201))
    c1.send_raw(cs_chat("연결유지확인"))
    nk, tx = parse_sc_chat(c1.recv_expect(SC_CHAT))
    assert nk == "테스터01" and tx == "연결유지확인"

    # 4b) 미정의 패킷 999 -> 차단
    c_undef = TestClient(port)
    c_undef.send_raw(frame_packet(999, b"\x08\x01"))
    assert c_undef.expect_closed(), "Undefined packet 999 should close connection"
    c_undef.close()

    # 4c) 깨진 Protobuf -> 차단
    c_corrupt = TestClient(port)
    c_corrupt.send_raw(frame_packet(CS_LOGIN, b"\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\xFF\x02"))
    assert c_corrupt.expect_closed(), "Corrupt protobuf should close connection"
    c_corrupt.close()

    # 4d) 헤더 크기 1021B 초과 -> 차단
    c_over = TestClient(port)
    c_over.send_raw(struct.pack("<HH", CS_CHAT, PACKET_MAX_BODY_SIZE + 1))
    assert c_over.expect_closed(), "Oversized header (1021B) should close connection"
    c_over.close()

    c1.close()
    # 방 비움 대기
    time.sleep(0.1)
    print("[PASS] 4. 경고·차단 정책 5종 (빈/201자 유지 · 999/깨진Protobuf/1021B 차단)")

    # 5. 다중 동시 부하 (N명 동시 로그인 + 30% 중도 이탈 + M건 채팅 무손실)
    t0 = time.perf_counter()

    def connect_and_login(idx: int) -> TestClient:
        cl = TestClient(port, timeout=10.0)
        cl.send_raw(cs_login(f"Load{idx:04d}"))
        r, _ = parse_sc_login_result(cl.recv_expect(SC_LOGIN_RESULT, timeout=10.0))
        if r != LOGIN_SUCCESS:
            raise AssertionError(f"Load user {idx} login failed: {r}")
        return cl

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        clients = list(pool.map(connect_and_login, range(load_clients)))
    t_login = (time.perf_counter() - t0) * 1000.0

    leave_count = max(1, load_clients * 3 // 10)
    for cl in clients[:leave_count]:
        cl.close()
    survivors = clients[leave_count:]

    # 잔류 인원 안정화 확인
    expected_surv = len(survivors)
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        survivors[0].send_raw(cs_room_users(0))
        _, tot, _ = parse_sc_room_users(survivors[0].recv_expect(SC_ROOM_USERS))
        if tot == expected_surv:
            break
        time.sleep(0.05)
    assert tot == expected_surv, f"Expected {expected_surv} survivors, got {tot}"

    # 동시 채팅 브로드캐스트 무손실 검증
    t_chat0 = time.perf_counter()
    sender = survivors[0]
    for i in range(chat_count):
        sender.send_raw(cs_chat(f"LOAD_MSG_{i:04d}"))

    def verify_receiver(cl: TestClient) -> int:
        received = 0
        while received < chat_count:
            _, txt = parse_sc_chat(cl.recv_expect(SC_CHAT, timeout=10.0))
            if txt.startswith("LOAD_MSG_"):
                received += 1
        return received

    with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
        counts = list(pool.map(verify_receiver, survivors))
    t_chat = (time.perf_counter() - t_chat0) * 1000.0
    total_delivered = sum(counts)
    assert all(c == chat_count for c in counts), "Chat delivery loss detected"

    for cl in survivors:
        cl.close()
    time.sleep(0.1)

    stats["load_clients"] = load_clients
    stats["load_login_ms"] = round(t_login, 2)
    stats["survivors"] = expected_surv
    stats["chat_delivered"] = total_delivered
    stats["chat_ms"] = round(t_chat, 2)
    stats["chat_kmsg_s"] = round((total_delivered / max(t_chat, 0.001)), 2)
    print(
        f"[PASS] 5. 다중 부하 ({load_clients}명 로그인 {t_login:.1f} ms / "
        f"{leave_count}명 이탈 / 잔류 {expected_surv}명 × {chat_count}건 = "
        f"{total_delivered:,}건 무손실, {t_chat:.1f} ms, {stats['chat_kmsg_s']:.2f} Kmsg/s)"
    )

    # 6. SlowReader 1 MiB 송신 큐 초과 백프레셔 강제 차단
    slow = TestClient(port)
    slow.sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    slow.send_raw(cs_login("SlowReader"))
    r_slow, _ = parse_sc_login_result(slow.recv_expect(SC_LOGIN_RESULT))
    assert r_slow == LOGIN_SUCCESS

    flooder = TestClient(port)
    flooder.send_raw(cs_login("FloodSender"))
    r_fl, _ = parse_sc_login_result(flooder.recv_expect(SC_LOGIN_RESULT))
    assert r_fl == LOGIN_SUCCESS

    # SlowReader는 recv하지 않은 채 FloodSender만 자신의 에코를 소비하면서 200한글자(600B+) 채팅 송신
    big_msg = cs_chat("가" * 200)
    flooder.sock.setblocking(False)
    for idx in range(2600):
        # FloodSender 자신은 백프레셔에 걸리지 않도록 송신 틈틈이 수신 버퍼를 비운다.
        try:
            while True:
                drained = flooder.sock.recv(65536)
                if not drained:
                    break
        except BlockingIOError:
            pass
        flooder.sock.setblocking(True)
        flooder.send_raw(big_msg)
        flooder.sock.setblocking(False)
    flooder.sock.setblocking(True)
    flooder.sock.settimeout(5.0)
    flooder._buf.clear()

    # SlowReader 연결이 실제로 서버에 의해 차단됐는지 + 방 인원이 1명(FloodSender만 생존)인지 확인
    assert slow.expect_closed(timeout=5.0), "SlowReader socket was not closed by 1 MiB backpressure"

    # FloodSender 잔여 에코를 비우고 방 인원 확인
    deadline = time.monotonic() + 8.0
    slow_kicked = False
    while time.monotonic() < deadline:
        flooder.sock.setblocking(False)
        try:
            while True:
                if not flooder.sock.recv(65536):
                    break
        except BlockingIOError:
            pass
        flooder.sock.setblocking(True)
        flooder.sock.settimeout(2.0)
        flooder._buf.clear()
        flooder.send_raw(cs_room_users(0))
        try:
            _, cur_tot, cur_nicks = parse_sc_room_users(flooder.recv_expect(SC_ROOM_USERS, timeout=1.0))
            if cur_tot == 1 and cur_nicks == ["FloodSender"]:
                slow_kicked = True
                break
        except (TimeoutError, ValueError):
            time.sleep(0.05)
    assert slow_kicked, "SlowReader was not removed from room after exceeding 1 MiB send queue"
    slow.close()
    flooder.close()
    print("[PASS] 6. SlowReader 1 MiB 송신 큐 초과 백프레셔 강제 차단")

    return stats


def verify_trace_files(exe_path: Path, pid: int, trace_mode: str) -> Dict[str, object]:
    info: Dict[str, object] = {"trace_mode": trace_mode}
    if trace_mode == "off":
        return info

    candidates = [
        exe_path.parent / "ChangeViewerTrace",
        exe_path.parent.parent / "ChangeViewerTrace",
    ]
    cvtrace_files: List[Path] = []
    cvlog_files: List[Path] = []
    for trace_dir in candidates:
        if trace_dir.is_dir():
            cvtrace_files.extend(sorted(trace_dir.glob(f"*-{pid}.cvtrace")))
            cvlog_files.extend(sorted(trace_dir.glob(f"*-{pid}.cvlog")))
    assert cvtrace_files, f"No .cvtrace file found for PID {pid} in {[str(c) for c in candidates]}"
    assert cvlog_files, f"No .cvlog file found for PID {pid} in {[str(c) for c in candidates]}"

    trace_path = cvtrace_files[-1]
    log_path = cvlog_files[-1]
    raw = trace_path.read_bytes()
    assert raw[:8] == b"CVTRACE1" and len(raw) > 64, f"Invalid .cvtrace header: {trace_path}"

    log_lines = [json.loads(line) for line in log_path.read_text(encoding="utf-8").splitlines() if line.strip()]
    joined_msgs = "\n".join(str(x.get("msg", "")) for x in log_lines)

    expected_markers = [
        "empty",
        "exceeded 200 chars",
        "undefined packet type",
        "parse failed",
        "oversized packet header",
        "send queue exceeded limit",
    ]
    missing = [m for m in expected_markers if m not in joined_msgs]
    assert not missing, f"Missing expected warning markers in .cvlog: {missing}"

    info["cvtrace_bytes"] = len(raw)
    info["cvlog_records"] = len(log_lines)
    print(
        f"[PASS] 8. 런타임 관측 무결성 (.cvtrace {len(raw):,} B / "
        f".cvlog {len(log_lines)}건 / 경고 마커 6종 전수 확인)"
    )
    return info


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    default_exe = repo_root / "Server" / "Bin" / "Release" / "ServerApp.exe"

    parser = argparse.ArgumentParser(description="MultiServer 원클릭 E2E · 부하 · 백프레셔 재현 검증")
    parser.add_argument("--exe", type=Path, default=default_exe, help="ServerApp.exe 경로")
    parser.add_argument("--workers", type=int, default=4, help="네트워크 워커 수")
    parser.add_argument("--load-clients", type=int, default=100, help="다중 부하 클라이언트 수")
    parser.add_argument("--chat-count", type=int, default=100, help="부하 채팅 메시지 수")
    parser.add_argument(
        "--trace",
        choices=["stream", "flight", "off"],
        default="stream",
        help="ServerApp 트레이스 모드 (기본: stream)",
    )
    args = parser.parse_args()

    exe_path = args.exe.resolve()
    if not exe_path.is_file():
        print(f"[FAIL] ServerApp.exe not found: {exe_path}", file=sys.stderr)
        return 1

    port = find_free_port()
    cmd = [str(exe_path), str(args.workers), str(port), f"--trace={args.trace}"]
    print(f"=== MultiServer E2E Verify ===")
    print(f"EXE   : {exe_path}")
    print(f"CMD   : {' '.join(cmd)}")

    proc = subprocess.Popen(
        cmd,
        cwd=str(exe_path.parent),
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    try:
        wait_port_ready(port, timeout=5.0)
        stats = run_scenarios(port, args.load_clients, args.chat_count)

        # Enter 입력으로 정상 종료 요청
        assert proc.stdin is not None
        proc.stdin.write(b"\n")
        proc.stdin.flush()
        stdout_b, stderr_b = proc.communicate(timeout=10.0)
        assert proc.returncode == 0, (
            f"ServerApp exited with {proc.returncode}\n"
            f"STDERR:\n{stderr_b.decode('utf-8', errors='replace')}"
        )
        assert not stderr_b.strip(), f"Unexpected stderr: {stderr_b.decode('utf-8', errors='replace')}"
        print(f"[PASS] 7. ServerApp 정상 종료 (rc=0, stderr=0B)")

        trace_info = verify_trace_files(exe_path, proc.pid, args.trace)
        print("=== ALL CHECKS PASSED ===")
        print(json.dumps({**stats, **trace_info}, ensure_ascii=False))
        return 0
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5.0)


if __name__ == "__main__":
    raise SystemExit(main())
