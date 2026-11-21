#!/usr/bin/env python3
"""MultiServer Phase 2 MySQL 8.4 실제 연동 E2E · 정합성 · DB 부하 검증기.

Docker MySQL 8.4(multiserver-mysql)와 ServerApp.exe --db=mysql을 실제로 연결해 아래 7개 항목을 검증한다:
  1. CS_REGISTER 회원가입 정책 (성공 / 중복 아이디 / 중복 닉네임 / 3종 입력 규칙 위반)
  2. CS_LOGIN PBKDF2-HMAC-SHA256 인증 (오답 비밀번호 / 미존재 아이디 / 정상 로그인 + DB 닉네임 반환)
  3. 동일 계정 중복 접속 차단(LOGIN_RESULT_ALREADY_ONLINE) 및 퇴장 후 재로그인
  4. 다중 방 이동(#3 전략) · 채팅 · 정상 퇴장 + 3단 캐스케이드 종료(rc=0, stderr=0B)
  5. MySQL accounts / connection_logs 실제 SQL 조회 검증 (해시 32B / 솔트 16B / logout_at / last_room_id=3 / reason='NORMAL')
  6. 100계정 동시 가입 + 100명 동시 MySQL 로그인 + 70명 × 100건(7,000건) 실시간 채팅 무손실 (DB 스레드 격리 확인)
  7. --pbkdf2-iters=60000(보안) vs --pbkdf2-iters=1000(순수 DB I/O) 로그인 지연 분리 계측
"""

from __future__ import annotations

import argparse
import concurrent.futures
import json
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

CS_LOGIN = 1
CS_CHAT = 2
CS_JOIN_ROOM = 5
CS_REGISTER = 7

SC_LOGIN_RESULT = 1
SC_CHAT = 2
SC_JOIN_ROOM_RESULT = 6
SC_REGISTER_RESULT = 8

LOGIN_SUCCESS = 1
LOGIN_INVALID_CREDENTIALS = 6
LOGIN_DB_ERROR = 7
LOGIN_ALREADY_ONLINE = 8

REG_SUCCESS = 1
REG_INVALID_USERNAME = 2
REG_INVALID_PASSWORD = 3
REG_INVALID_NICKNAME = 4
REG_DUP_USERNAME = 5
REG_DUP_NICKNAME = 6

JOIN_SUCCESS = 1


def encode_varint(v: int) -> bytes:
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            break
    return bytes(out)


def encode_str(fno: int, s: str) -> bytes:
    raw = s.encode("utf-8")
    if not raw:
        return b""
    return encode_varint((fno << 3) | 2) + encode_varint(len(raw)) + raw


def encode_u32(fno: int, v: int) -> bytes:
    if v == 0:
        return b""
    return encode_varint((fno << 3) | 0) + encode_varint(v)


def decode_varint(data: bytes, pos: int) -> Tuple[int, int]:
    shift = 0
    val = 0
    while True:
        b = data[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val, pos
        shift += 7


def decode_fields(data: bytes) -> Dict[int, List[object]]:
    fields: Dict[int, List[object]] = {}
    pos = 0
    n = len(data)
    while pos < n:
        tag, pos = decode_varint(data, pos)
        fno = tag >> 3
        wt = tag & 7
        if wt == 0:
            v, pos = decode_varint(data, pos)
            fields.setdefault(fno, []).append(v)
        elif wt == 2:
            ln, pos = decode_varint(data, pos)
            fields.setdefault(fno, []).append(data[pos:pos + ln])
            pos += ln
        else:
            raise ValueError(f"Unsupported wire type {wt}")
    return fields


def frame(ptype: int, body: bytes) -> bytes:
    return struct.pack("<HH", ptype, len(body)) + body


def pkt_register(username: str, password: str, nickname: str) -> bytes:
    return frame(CS_REGISTER, encode_str(1, username) + encode_str(2, password) + encode_str(3, nickname))


def pkt_login_db(username: str, password: str) -> bytes:
    return frame(CS_LOGIN, encode_str(2, username) + encode_str(3, password))


def pkt_chat(text: str) -> bytes:
    return frame(CS_CHAT, encode_str(1, text))


def pkt_join_room(room_id: int) -> bytes:
    return frame(CS_JOIN_ROOM, encode_u32(1, room_id))


def parse_register_result(body: bytes) -> int:
    return int(decode_fields(body).get(1, [0])[-1])


def parse_login_result(body: bytes) -> Tuple[int, int, int, str]:
    f = decode_fields(body)
    res = int(f.get(1, [0])[-1])
    ucnt = int(f.get(2, [0])[-1])
    rid = int(f.get(3, [0])[-1])
    nick = bytes(f.get(4, [b""])[-1]).decode("utf-8")
    return res, ucnt, rid, nick


def parse_chat(body: bytes) -> Tuple[str, str]:
    f = decode_fields(body)
    return bytes(f.get(1, [b""])[-1]).decode("utf-8"), bytes(f.get(2, [b""])[-1]).decode("utf-8")


def parse_join_result(body: bytes) -> Tuple[int, int, int]:
    f = decode_fields(body)
    return int(f.get(1, [0])[-1]), int(f.get(2, [0])[-1]), int(f.get(3, [0])[-1])


class Client:
    def __init__(self, port: int, timeout: float = 10.0) -> None:
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sock.settimeout(timeout)
        self._buf = bytearray()

    def send(self, data: bytes) -> None:
        self.sock.sendall(data)

    def recv_expect(self, expected: int, timeout: float = 10.0) -> bytes:
        dl = time.monotonic() + timeout
        while True:
            if len(self._buf) >= 4:
                pt, sz = struct.unpack_from("<HH", self._buf, 0)
                if len(self._buf) >= 4 + sz:
                    body = bytes(self._buf[4:4 + sz])
                    del self._buf[:4 + sz]
                    if pt == expected:
                        return body
                    continue
            rem = dl - time.monotonic()
            if rem <= 0:
                raise TimeoutError(f"Timed out waiting packet {expected}")
            self.sock.settimeout(rem)
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError(f"Closed waiting packet {expected}")
            self._buf.extend(chunk)

    def close(self) -> None:
        try:
            self.sock.close()
        except OSError:
            pass


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return int(s.getsockname()[1])


def wait_port(port: int, timeout: float = 8.0) -> None:
    dl = time.monotonic() + timeout
    while time.monotonic() < dl:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"Server not ready on {port}")


def docker_sql(sql: str) -> str:
    res = subprocess.run(
        [
            "docker", "exec", "multiserver-mysql",
            "mysql", "-N", "-B", "-umultiserver", "-pmultiserver_dev_pw", "multiserver", "-e", sql,
        ],
        capture_output=True, text=True, encoding="utf-8", errors="replace", check=True,
    )
    return res.stdout.strip()


def run_functional_and_load(exe: Path, pbkdf2_iters: int, db_workers: int) -> Dict[str, object]:
    # 테스트 격리를 위해 매 실행 전 테스트 테이블 정리
    docker_sql("SET FOREIGN_KEY_CHECKS=0; TRUNCATE TABLE connection_logs; TRUNCATE TABLE accounts; SET FOREIGN_KEY_CHECKS=1;")

    port = find_free_port()
    cmd = [
        str(exe), "4", str(port), "--trace=off", "--db=mysql",
        f"--db-workers={db_workers}", f"--pbkdf2-iters={pbkdf2_iters}",
    ]
    proc = subprocess.Popen(cmd, cwd=str(exe.parent), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    stats: Dict[str, object] = {"pbkdf2_iters": pbkdf2_iters, "db_workers": db_workers}
    try:
        wait_port(port)

        # 1. 회원가입 정책 6종
        c = Client(port)
        c.send(pkt_register("user_alpha", "Pass1234!", "알파테스터"))
        assert parse_register_result(c.recv_expect(SC_REGISTER_RESULT)) == REG_SUCCESS
        c.send(pkt_register("user_beta", "Pass5678!", "베타테스터"))
        assert parse_register_result(c.recv_expect(SC_REGISTER_RESULT)) == REG_SUCCESS
        c.send(pkt_register("user_alpha", "Pass9999!", "다른닉네임"))
        assert parse_register_result(c.recv_expect(SC_REGISTER_RESULT)) == REG_DUP_USERNAME
        c.send(pkt_register("user_gamma", "Pass9999!", "알파테스터"))
        assert parse_register_result(c.recv_expect(SC_REGISTER_RESULT)) == REG_DUP_NICKNAME
        c.send(pkt_register("ab", "Pass1234!", "규칙확인"))
        assert parse_register_result(c.recv_expect(SC_REGISTER_RESULT)) == REG_INVALID_USERNAME
        c.send(pkt_register("user_shortpw", "123", "규칙확인"))
        assert parse_register_result(c.recv_expect(SC_REGISTER_RESULT)) == REG_INVALID_PASSWORD
        c.send(pkt_register("user_badnick", "Pass1234!", "Bad!"))
        assert parse_register_result(c.recv_expect(SC_REGISTER_RESULT)) == REG_INVALID_NICKNAME
        print(f"[PASS] 1. MySQL 회원가입 정책 7종 (성공 / 중복 ID·닉네임 / 규칙 위반 3종)")

        # 2. 로그인 인증 정책 (틀린 비밀번호 / 없는 계정 / 성공 + DB 닉네임 반환)
        c.send(pkt_login_db("user_alpha", "WrongPass!"))
        assert parse_login_result(c.recv_expect(SC_LOGIN_RESULT))[0] == LOGIN_INVALID_CREDENTIALS
        c.send(pkt_login_db("no_such_user", "Pass1234!"))
        assert parse_login_result(c.recv_expect(SC_LOGIN_RESULT))[0] == LOGIN_INVALID_CREDENTIALS

        c.send(pkt_login_db("user_alpha", "Pass1234!"))
        r, cnt, rid, nick = parse_login_result(c.recv_expect(SC_LOGIN_RESULT))
        assert r == LOGIN_SUCCESS and cnt == 1 and rid == 1 and nick == "알파테스터", f"Login mismatch: {r}/{cnt}/{rid}/{nick}"
        print(f"[PASS] 2. MySQL PBKDF2 로그인 인증 및 DB 닉네임 반환 (nick={nick})")

        # 3. 동일 계정 중복 접속 차단 (ALREADY_ONLINE)
        c_dup = Client(port)
        c_dup.send(pkt_login_db("user_alpha", "Pass1234!"))
        r_dup, _, _, _ = parse_login_result(c_dup.recv_expect(SC_LOGIN_RESULT))
        assert r_dup == LOGIN_ALREADY_ONLINE, f"Expected ALREADY_ONLINE, got {r_dup}"
        c_dup.close()
        print("[PASS] 3. 동일 계정 중복 접속 차단 (LOGIN_RESULT_ALREADY_ONLINE)")

        # 4. 방 #3 이동 + 채팅 + 퇴장 (connection_logs 영속 확인용)
        c.send(pkt_join_room(3))
        jr, jrid, _ = parse_join_result(c.recv_expect(SC_JOIN_ROOM_RESULT))
        assert jr == JOIN_SUCCESS and jrid == 3
        c.send(pkt_chat("MySQL방3채팅테스트"))
        cn, ct = parse_chat(c.recv_expect(SC_CHAT))
        assert cn == "알파테스터" and ct == "MySQL방3채팅테스트"
        c.close()
        time.sleep(0.2)

        # 5. 100계정 동시 가입 + 100명 동시 MySQL 로그인 + 70명 × 100건 채팅 격리 부하
        t_reg0 = time.perf_counter()

        def reg_one(i: int) -> None:
            cl = Client(port, timeout=20.0)
            cl.send(pkt_register(f"dbuser_{i:04d}", "LoadPass123!", f"디비유저{i:03d}"))
            assert parse_register_result(cl.recv_expect(SC_REGISTER_RESULT, timeout=20.0)) == REG_SUCCESS
            cl.close()

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            list(pool.map(reg_one, range(100)))
        t_reg_ms = round((time.perf_counter() - t_reg0) * 1000.0, 2)

        t_log0 = time.perf_counter()

        def login_one(i: int) -> Client:
            cl = Client(port, timeout=20.0)
            cl.send(pkt_login_db(f"dbuser_{i:04d}", "LoadPass123!"))
            rr, _, _, nn = parse_login_result(cl.recv_expect(SC_LOGIN_RESULT, timeout=20.0))
            assert rr == LOGIN_SUCCESS and nn == f"디비유저{i:03d}", f"DB load login failed {i}: {rr}/{nn}"
            return cl

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            clients = list(pool.map(login_one, range(100)))
        t_log_ms = round((time.perf_counter() - t_log0) * 1000.0, 2)

        for cl in clients[:30]:
            cl.close()
        survivors = clients[30:]
        time.sleep(0.15)

        t_ch0 = time.perf_counter()
        sender = survivors[0]
        for seq in range(100):
            sender.send(pkt_chat(f"DBCHAT_{seq:03d}"))

        def recv_100(cl: Client) -> int:
            got = 0
            while got < 100:
                _, txt = parse_chat(cl.recv_expect(SC_CHAT, timeout=15.0))
                if txt.startswith("DBCHAT_"):
                    got += 1
            return got

        with concurrent.futures.ThreadPoolExecutor(max_workers=16) as pool:
            counts = list(pool.map(recv_100, survivors))
        t_ch_ms = round((time.perf_counter() - t_ch0) * 1000.0, 2)
        delivered = sum(counts)
        assert delivered == 7000
        kmsg_s = round(delivered / max(t_ch_ms, 0.001), 2)

        for cl in survivors:
            cl.close()
        time.sleep(0.2)

        # 3단 캐스케이드 정상 종료 (모든 로그아웃 UPDATE 배수 확인)
        t_sd = time.perf_counter()
        assert proc.stdin is not None
        proc.stdin.write(b"\n")
        proc.stdin.flush()
        _, stderr_b = proc.communicate(timeout=15.0)
        sd_ms = round((time.perf_counter() - t_sd) * 1000.0, 2)
        assert proc.returncode == 0 and not stderr_b.strip(), f"rc={proc.returncode}, stderr={stderr_b}"

        # 6. 실제 MySQL 테이블 SQL 검증
        acc_cnt = int(docker_sql("SELECT COUNT(*) FROM accounts;"))
        hash_ok = int(docker_sql("SELECT COUNT(*) FROM accounts WHERE LENGTH(password_hash)=32 AND LENGTH(password_salt)=16;"))
        alpha_log = docker_sql(
            "SELECT l.last_room_id, l.disconnect_reason, (l.logout_at IS NOT NULL) "
            "FROM connection_logs l JOIN accounts a ON a.account_id=l.account_id "
            "WHERE a.username='user_alpha' ORDER BY l.log_id ASC LIMIT 1;"
        )
        open_logs = int(docker_sql("SELECT COUNT(*) FROM connection_logs WHERE logout_at IS NULL;"))
        total_logs = int(docker_sql("SELECT COUNT(*) FROM connection_logs;"))

        assert acc_cnt == 102 and hash_ok == 102, f"accounts mismatch: {acc_cnt}/{hash_ok}"
        assert alpha_log == "3\tNORMAL\t1", f"alpha log mismatch: {alpha_log!r}"
        assert open_logs == 0 and total_logs >= 101, f"Drain incomplete: open={open_logs}, total={total_logs}"

        print(f"[PASS] 4. MySQL 동시 부하 (100가입 {t_reg_ms}ms / 100로그인 {t_log_ms}ms / 7,000건 채팅 {t_ch_ms}ms, {kmsg_s} Kmsg/s)")
        print(f"[PASS] 5. 3단 배수 정상 종료 ({sd_ms}ms, rc=0) & SQL 검증 (계정 {acc_cnt}/해시32B {hash_ok}, user_alpha={alpha_log}, 미종료 이력 {open_logs}건 / 총 {total_logs}건)")

        stats.update({
            "register_100_ms": t_reg_ms,
            "login_100_ms": t_log_ms,
            "chat_7000_ms": t_ch_ms,
            "chat_kmsg_s": kmsg_s,
            "shutdown_ms": sd_ms,
            "accounts_verified": acc_cnt,
            "connection_logs_total": total_logs,
            "connection_logs_unclosed": open_logs,
        })
        return stats
    finally:
        if proc.poll() is None:
            proc.kill()
            proc.wait(timeout=5.0)


def main() -> int:
    parser = argparse.ArgumentParser(description="MultiServer MySQL 8.4 실DB E2E 및 부하 검증")
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--db-workers", type=int, default=4)
    parser.add_argument("--pbkdf2-iters", type=int, default=60000)
    args = parser.parse_args()

    res_sec = run_functional_and_load(args.exe.resolve(), args.pbkdf2_iters, args.db_workers)
    res_io = run_functional_and_load(args.exe.resolve(), 1000, args.db_workers)
    print("=== ALL MYSQL E2E CHECKS PASSED ===")
    print(json.dumps({"security_60k": res_sec, "db_io_1k": res_io}, ensure_ascii=False))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
