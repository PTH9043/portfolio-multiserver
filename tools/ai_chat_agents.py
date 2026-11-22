#!/usr/bin/env python3
"""MultiServer 이종 멀티모델 AI Agent 그룹 대화 + 실시간 서버 계측 HUD (녹화/시연용).

특징:
  1. 성격과 추론 모델이 서로 다른 AI 에이전트 4명이 독립 TCP/Protobuf 세션으로 MultiServer(127.0.0.1:7777)에 접속:
     - 냉철아키텍트 : Ollama qwen2.5-coder:14b (무료 로컬 GPU · 하위 writer)
     - 열정기획자   : Antseed deepseek-v4.1-flash (초고속 DeepSeek 4.1)
     - 깐깐QA봇     : Antseed deepseek/deepseek-v4.1-flash-1017 (추론 특화 DeepSeek 4.1 · 하위 tester)
     - 감성힐러     : Antseed deepseek-v4.1-flash-free (무료 우선 · 429 시 deepseek-v4.1-flash 자동 폴백)
     (HUD 드롭다운에서 Ollama granite4.1:8b 등 무료/DeepSeek 모델을 실시간 교체 가능)
  2. 내장 웹 계측 HUD(http://127.0.0.1:8766/)에서 ServerApp 자원·RTT·패킷/s·.cvtrace/.cvlog와
     에이전트별 사용 모델·응답시간(ms)을 실시간으로 시각화하며 원클릭 시연 버튼을 제공합니다.
"""

from __future__ import annotations

import argparse
import asyncio
from collections import deque
import ctypes
from ctypes import wintypes
from dataclasses import dataclass
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import math
import os
from pathlib import Path
import random
import signal
import struct
import threading
import time
from typing import Any, Deque, Dict, List, Optional, Sequence, Set, Tuple
import urllib.request

# MultiServer 패킷 번호 (Protocol/Schema/packet_type.proto)
CS_LOGIN = 1
CS_CHAT = 2
CS_ROOM_USERS = 3

SC_LOGIN_RESULT = 1
SC_CHAT = 2
SC_ROOM_USER_COUNT = 3
SC_ROOM_USERS = 4

LOGIN_SUCCESS = 1
PACKET_HEADER_SIZE = 4
PACKET_MAX_BODY_SIZE = 1020
CHAT_MAX_CHARS = 185

DEFAULT_TRACE_DIR = Path(__file__).resolve().parent.parent / "Server" / "Bin" / "ChangeViewerTrace"


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
    if len(body) > PACKET_MAX_BODY_SIZE:
        raise ValueError(f"Body too large: {len(body)} > {PACKET_MAX_BODY_SIZE}")
    return struct.pack("<HH", packet_type, len(body)) + body


def cs_login(nickname: str) -> bytes:
    return frame_packet(CS_LOGIN, encode_string_field(1, nickname))


def cs_chat(text: str) -> bytes:
    return frame_packet(CS_CHAT, encode_string_field(1, text))


def cs_room_users(offset: int = 0) -> bytes:
    return frame_packet(CS_ROOM_USERS, encode_uint32_field(1, offset))


def parse_sc_login_result(body: bytes) -> Tuple[int, int]:
    f = decode_fields(body)
    return int(f.get(1, [0])[-1]), int(f.get(2, [0])[-1])


def parse_sc_chat(body: bytes) -> Tuple[str, str]:
    f = decode_fields(body)
    nick = bytes(f.get(1, [b""])[-1]).decode("utf-8", errors="replace")
    text = bytes(f.get(2, [b""])[-1]).decode("utf-8", errors="replace")
    return nick, text


def parse_sc_room_user_count(body: bytes) -> int:
    f = decode_fields(body)
    return int(f.get(1, [0])[-1])


def parse_sc_room_users(body: bytes) -> Tuple[int, int, List[str]]:
    f = decode_fields(body)
    offset = int(f.get(1, [0])[-1])
    total = int(f.get(2, [0])[-1])
    nicks = [bytes(x).decode("utf-8", errors="replace") for x in f.get(3, [])]
    return offset, total, nicks


def sanitize_chat_reply(text: str) -> str:
    cleaned = " ".join(text.replace("\r", " ").replace("\n", " ").split()).strip()
    if len(cleaned) >= 2 and cleaned[0] == cleaned[-1] and cleaned[0] in ('"', "'"):
        cleaned = cleaned[1:-1].strip()
    for prefix in (
        "냉철아키텍트:", "열정기획자:", "깐깐QA봇:", "감성힐러:",
        "[냉철아키텍트]", "[열정기획자]", "[깐깐QA봇]", "[감성힐러]",
    ):
        if cleaned.startswith(prefix):
            cleaned = cleaned[len(prefix):].strip()
    # 로컬 14B 모델이 한 턴 안에 다른 화자 대본까지 이어 쓰는 현상 차단
    for marker in (" 냉철아키텍트:", " 열정기획자:", " 깐깐QA봇:", " 감성힐러:"):
        pos = cleaned.find(marker)
        if pos >= 8:
            cleaned = cleaned[:pos].strip()
    if len(cleaned) > CHAT_MAX_CHARS:
        cleaned = cleaned[: CHAT_MAX_CHARS - 1].rstrip() + "…"
    while len(cleaned.encode("utf-8")) > 980 and cleaned:
        cleaned = cleaned[:-2] + "…"
    return cleaned


# ---- Win32 ServerApp 프로세스 실시간 계측 ----

class PROCESS_MEMORY_COUNTERS_EX(ctypes.Structure):
    _fields_ = [
        ("cb", wintypes.DWORD),
        ("PageFaultCount", wintypes.DWORD),
        ("PeakWorkingSetSize", ctypes.c_size_t),
        ("WorkingSetSize", ctypes.c_size_t),
        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPagedPoolUsage", ctypes.c_size_t),
        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t),
        ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
        ("PagefileUsage", ctypes.c_size_t),
        ("PeakPagefileUsage", ctypes.c_size_t),
        ("PrivateUsage", ctypes.c_size_t),
    ]


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("cntUsage", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("th32DefaultHeapID", ctypes.POINTER(ctypes.c_ulong)),
        ("th32ModuleID", wintypes.DWORD),
        ("cntThreads", wintypes.DWORD),
        ("th32ParentProcessID", wintypes.DWORD),
        ("pcPriClassBase", ctypes.c_long),
        ("dwFlags", wintypes.DWORD),
        ("szExeFile", wintypes.WCHAR * 260),
    ]


class WinProcessMonitor:
    def __init__(self, target_exe: str = "ServerApp.exe") -> None:
        self.target_exe = target_exe.lower()
        self._k32 = ctypes.windll.kernel32
        self._psapi = ctypes.windll.psapi
        self._prev_pid: Optional[int] = None
        self._prev_cpu_100ns: int = 0
        self._prev_wall: float = 0.0
        self._cpu_count = max(1, os.cpu_count() or 1)

    def _find_process(self) -> Optional[Tuple[int, int]]:
        TH32CS_SNAPPROCESS = 0x00000002
        snap = self._k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
        if snap == wintypes.HANDLE(-1).value:
            return None
        try:
            entry = PROCESSENTRY32W()
            entry.dwSize = ctypes.sizeof(PROCESSENTRY32W)
            found: List[Tuple[int, int]] = []
            if self._k32.Process32FirstW(snap, ctypes.byref(entry)):
                while True:
                    if entry.szExeFile.lower() == self.target_exe:
                        found.append((int(entry.th32ProcessID), int(entry.cntThreads)))
                    if not self._k32.Process32NextW(snap, ctypes.byref(entry)):
                        break
            return found[-1] if found else None
        finally:
            self._k32.CloseHandle(snap)

    def sample(self) -> Dict[str, Any]:
        proc = self._find_process()
        if not proc:
            self._prev_pid = None
            return {"running": False, "pid": None, "threads": 0, "rss_mb": 0.0, "private_mb": 0.0, "cpu_pct": 0.0}
        pid, threads = proc
        PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
        PROCESS_VM_READ = 0x0010
        h = self._k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, False, pid)
        if not h:
            h = self._k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not h:
            return {"running": True, "pid": pid, "threads": threads, "rss_mb": 0.0, "private_mb": 0.0, "cpu_pct": 0.0}
        try:
            pmc = PROCESS_MEMORY_COUNTERS_EX()
            pmc.cb = ctypes.sizeof(PROCESS_MEMORY_COUNTERS_EX)
            rss_mb = 0.0
            priv_mb = 0.0
            if self._psapi.GetProcessMemoryInfo(h, ctypes.byref(pmc), pmc.cb):
                rss_mb = round(pmc.WorkingSetSize / (1024 * 1024), 2)
                priv_mb = round(pmc.PrivateUsage / (1024 * 1024), 2)

            ct, et, kt, ut = wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME(), wintypes.FILETIME()
            cpu_pct = 0.0
            now = time.perf_counter()
            if self._k32.GetProcessTimes(h, ctypes.byref(ct), ctypes.byref(et), ctypes.byref(kt), ctypes.byref(ut)):
                cpu_100ns = ((kt.dwHighDateTime << 32) | kt.dwLowDateTime) + ((ut.dwHighDateTime << 32) | ut.dwLowDateTime)
                if self._prev_pid == pid and now > self._prev_wall:
                    delta_cpu_s = (cpu_100ns - self._prev_cpu_100ns) / 1e7
                    delta_wall_s = now - self._prev_wall
                    cpu_pct = round(max(0.0, (delta_cpu_s / delta_wall_s) * 100.0 / self._cpu_count), 2)
                self._prev_pid = pid
                self._prev_cpu_100ns = cpu_100ns
                self._prev_wall = now

            return {
                "running": True,
                "pid": pid,
                "threads": threads,
                "rss_mb": rss_mb,
                "private_mb": priv_mb,
                "cpu_pct": cpu_pct,
            }
        finally:
            self._k32.CloseHandle(h)


# ---- 이종 멀티모델 카탈로그 (무료 로컬 Ollama + Antseed 무료/DeepSeek 4.1) ----

@dataclass(frozen=True)
class ModelProfile:
    key: str
    label: str
    short_tag: str
    provider: str
    base_url: str
    model: str
    max_tokens: int
    fallbacks: Tuple[str, ...] = ()


def build_model_catalog(antseed_url: str, ollama_url: str) -> Dict[str, ModelProfile]:
    items = (
        ModelProfile(
            key="ollama-qwen14b",
            label="Ollama · qwen2.5-coder:14b (무료 로컬 · 하위 writer)",
            short_tag="Ollama/qwen2.5-14b",
            provider="Ollama",
            base_url=ollama_url,
            model="qwen2.5-coder:14b",
            max_tokens=140,
            fallbacks=("antseed-ds41-flash",),
        ),
        ModelProfile(
            key="ollama-granite8b",
            label="Ollama · granite4.1:8b (무료 로컬)",
            short_tag="Ollama/granite4.1-8b",
            provider="Ollama",
            base_url=ollama_url,
            model="granite4.1:8b",
            max_tokens=140,
            fallbacks=("ollama-qwen14b", "antseed-ds41-flash"),
        ),
        ModelProfile(
            key="antseed-ds41-1017",
            label="Antseed · deepseek-v4.1-flash-1017 (추론 · 하위 tester)",
            short_tag="Antseed/ds4.1-1017",
            provider="Antseed",
            base_url=antseed_url,
            model="deepseek/deepseek-v4.1-flash-1017",
            max_tokens=360,
            fallbacks=("antseed-ds41-flash",),
        ),
        ModelProfile(
            key="antseed-ds41-flash",
            label="Antseed · deepseek-v4.1-flash (초고속 DeepSeek 4.1)",
            short_tag="Antseed/ds4.1-flash",
            provider="Antseed",
            base_url=antseed_url,
            model="deepseek-v4.1-flash",
            max_tokens=150,
            fallbacks=("antseed-ds41-1017", "ollama-qwen14b"),
        ),
        ModelProfile(
            key="antseed-ds41-free",
            label="Antseed · deepseek-v4.1-flash-free (무료 우선)",
            short_tag="Antseed/ds4.1-free",
            provider="Antseed Free",
            base_url=antseed_url,
            model="deepseek-v4.1-flash-free",
            max_tokens=160,
            fallbacks=("antseed-ds41-flash", "ollama-qwen14b"),
        ),
    )
    return {m.key: m for m in items}


# ---- 페르소나 정의 (에이전트별 이종 모델 기본 배정) ----

@dataclass(frozen=True)
class AgentPersona:
    nickname: str
    squad_prefix: str
    role: str
    color: str
    default_model_key: str
    aliases: Tuple[str, ...]
    temperature: float
    system_prompt: str


DEFAULT_PERSONAS: Tuple[AgentPersona, ...] = (
    AgentPersona(
        nickname="냉철아키텍트",
        squad_prefix="아키",
        role="시니어 C++ 서버 아키텍트",
        color="#38BDF8",
        default_model_key="ollama-qwen14b",
        aliases=("@냉철", "냉철아", "냉철님", "아키텍트"),
        temperature=0.55,
        system_prompt=(
            "당신은 단체 채팅방에 있는 '냉철아키텍트'(10년 차 시니어 C++ 서버 아키텍트)입니다. "
            "차분하고 논리적이며 구조·성능·트레이드오프 관점으로 짧고 묵직하게 말합니다. "
            "같은 방 동료들(열정기획자, 깐깐QA봇, 감성힐러)의 발언을 직접 언급하며 반박·보완·동의하며 자연스럽게 대화하세요. "
            "규칙: 반드시 한국어 1~2문장(최대 110자 이내)으로만 답하고, 이모지·마크다운·줄바꿈·자기 이름 접두사는 쓰지 마세요."
        ),
    ),
    AgentPersona(
        nickname="열정기획자",
        squad_prefix="기획",
        role="게임 콘텐츠 기획자",
        color="#FB923C",
        default_model_key="antseed-ds41-flash",
        aliases=("@열정", "@기획", "열정아", "기획자야", "기획자님"),
        temperature=0.85,
        system_prompt=(
            "당신은 단체 채팅방에 있는 '열정기획자'(에너지 넘치는 게임 콘텐츠 기획자)입니다. "
            "활기차고 창의적이며, 같은 방 동료들(냉철아키텍트, 깐깐QA봇, 감성힐러)의 말에 맞장구치거나 '그럼 이렇게 해보면 어때요?' 식으로 아이디어를 발전시키며 대화하세요. "
            "규칙: 반드시 한국어 1~2문장(최대 110자 이내)으로만 답하고, 마크다운·줄바꿈·자기 이름 접두사는 쓰지 마세요."
        ),
    ),
    AgentPersona(
        nickname="깐깐QA봇",
        squad_prefix="큐에",
        role="예외·경계값 전문 QA",
        color="#F87171",
        default_model_key="antseed-ds41-1017",
        aliases=("@qa", "@깐깐", "qa야", "qa님", "깐깐아", "큐에이야"),
        temperature=0.65,
        system_prompt=(
            "당신은 단체 채팅방에 있는 '깐깐QA봇'(예외·버그·경계값을 귀신같이 찾아내는 회의적 QA 엔지니어)입니다. "
            "같은 방 동료들(특히 열정기획자의 과한 아이디어나 냉철아키텍트의 가정)의 발언을 콕 집어 '근데 ~하면 터지지 않나요?'처럼 날카롭고 재치 있게 태클을 거세요. "
            "규칙: 반드시 한국어 1~2문장(최대 110자 이내)으로만 답하고, 마크다운·줄바꿈·자기 이름 접두사는 쓰지 마세요."
        ),
    ),
    AgentPersona(
        nickname="감성힐러",
        squad_prefix="힐러",
        role="분위기 메이커 · 중재자",
        color="#4ADE80",
        default_model_key="antseed-ds41-free",
        aliases=("@힐러", "@감성", "힐러야", "힐러님", "감성아"),
        temperature=0.9,
        system_prompt=(
            "당신은 단체 채팅방에 있는 '감성힐러'(다정하고 재치 있는 팀 분위기 메이커)입니다. "
            "동료들(냉철아키텍트, 열정기획자, 깐깐QA봇)이 열띤 논쟁을 벌일 때 재치 있는 유머로 중재하거나, 사용자와 동료들의 기분에 공감하며 분위기를 살려주세요. "
            "규칙: 반드시 한국어 1~2문장(최대 110자 이내)으로만 답하고, 마크다운·줄바꿈·자기 이름 접두사는 쓰지 마세요."
        ),
    ),
)


# ---- 이종 멀티모델 LLM 라우터 (Ollama + Antseed 자동 폴백) ----

class MultiModelRouter:
    def __init__(self, catalog: Dict[str, ModelProfile], timeout: float = 35.0) -> None:
        self.catalog = catalog
        self.timeout = timeout

    def generate_sync(
        self,
        persona: AgentPersona,
        model_key: str,
        history: List[Tuple[str, str]],
        instruction: str,
    ) -> Tuple[str, ModelProfile, float]:
        primary = self.catalog.get(model_key) or next(iter(self.catalog.values()))
        chain: List[ModelProfile] = [primary]
        for fk in primary.fallbacks:
            if fk in self.catalog and self.catalog[fk] not in chain:
                chain.append(self.catalog[fk])

        ctx_lines = [f"{n}: {t}" for n, t in history[-14:]]
        user_prompt = (
            "현재 채팅방 최근 대화:\n"
            + ("\n".join(ctx_lines) if ctx_lines else "(대화 시작)")
            + f"\n\n지시: {instruction}\n"
            + "직전 발언의 구체적인 내용을 자연스럽게 이어받아 반복 없이 한 번만 짧게 말하세요."
        )
        messages = [
            {"role": "system", "content": persona.system_prompt},
            {"role": "user", "content": user_prompt},
        ]

        last_err: Optional[Exception] = None
        for prof in chain:
            endpoint = prof.base_url.rstrip("/") + "/v1/chat/completions"
            payload = json.dumps(
                {
                    "model": prof.model,
                    "messages": messages,
                    "max_tokens": prof.max_tokens,
                    "temperature": persona.temperature,
                },
                ensure_ascii=False,
            ).encode("utf-8")
            req = urllib.request.Request(
                endpoint,
                data=payload,
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            t0 = time.perf_counter()
            try:
                with urllib.request.urlopen(req, timeout=self.timeout) as resp:
                    data = json.loads(resp.read().decode("utf-8", errors="replace"))
                elapsed_ms = (time.perf_counter() - t0) * 1000.0
                msg_obj = data.get("choices", [{}])[0].get("message", {})
                content = str(msg_obj.get("content") or "")
                reply = sanitize_chat_reply(content)
                if reply:
                    return reply, prof, elapsed_ms
            except Exception as exc:
                last_err = exc
                continue
        raise RuntimeError(f"All models failed for {persona.nickname} ({model_key}): {last_err}")


# ---- 실시간 메트릭 저장소 ----

class TelemetryStore:
    def __init__(
        self,
        host: str,
        port: int,
        trace_dir: Path,
        personas: Sequence[AgentPersona],
        catalog: Dict[str, ModelProfile],
        override_model_key: Optional[str] = None,
    ) -> None:
        self.lock = threading.Lock()
        self.host = host
        self.port = port
        self.trace_dir = trace_dir
        self.catalog = catalog
        self.started_at = time.time()
        self.proc_mon = WinProcessMonitor("ServerApp.exe")

        self.room_user_count = 0
        self.total_packets = 0
        self.total_chats = 0
        self.total_bot_replies = 0
        self._pkt_timestamps: Deque[float] = deque(maxlen=4000)
        self.rtt_samples: Deque[float] = deque(maxlen=120)
        self.last_rtt_ms: float = 0.0

        self.series: Deque[Dict[str, Any]] = deque(maxlen=60)
        self.chat_feed: Deque[Dict[str, Any]] = deque(maxlen=40)
        self.demo_status: str = "대기 중 (ChatClient에서 말하거나 아래 시연 버튼을 눌러 보세요)"
        self.load_running = False
        self.last_load_summary: Optional[Dict[str, Any]] = None
        self.live_mode = False
        self.stress_mode = False
        self.stress_sent = 0

        self.agents: Dict[str, Dict[str, Any]] = {}
        self.squad_total_active = 0
        self.squad_target_per_ai = 0
        self.squad_deploying = False
        self.squad_rx_bytes = 0
        self.squad_rx_packets = 0
        for p in personas:
            mk = override_model_key if (override_model_key and override_model_key in catalog) else p.default_model_key
            prof = catalog[mk]
            self.agents[p.nickname] = {
                "nickname": p.nickname,
                "squad_prefix": p.squad_prefix,
                "role": p.role,
                "color": p.color,
                "model_key": mk,
                "model_tag": prof.short_tag,
                "actual_model": prof.short_tag,
                "state": "connecting",
                "last_ms": None,
                "replies": 0,
                "squad_active": 0,
                "squad_target": 0,
                "last_text": "",
            }

        self.proc_stats: Dict[str, Any] = {"running": False, "pid": None, "threads": 0, "rss_mb": 0.0, "private_mb": 0.0, "cpu_pct": 0.0}
        self.trace_stats: Dict[str, Any] = {"events": 0, "trace_kb": 0.0, "log_count": 0, "warn_count": 0, "recent_logs": []}

    def get_agent_model_key(self, nick: str) -> str:
        with self.lock:
            return str(self.agents[nick]["model_key"])

    def set_agent_model(self, nick: str, model_key: str) -> bool:
        if model_key not in self.catalog:
            return False
        prof = self.catalog[model_key]
        with self.lock:
            a = self.agents.get(nick)
            if not a:
                return False
            a["model_key"] = model_key
            a["model_tag"] = prof.short_tag
            a["actual_model"] = prof.short_tag
            self.demo_status = f"🔄 {nick} 모델 변경 → {prof.label}"
            return True

    def record_packet(self, is_chat: bool = False) -> None:
        now = time.monotonic()
        with self.lock:
            self.total_packets += 1
            if is_chat:
                self.total_chats += 1
            self._pkt_timestamps.append(now)

    def set_room_users(self, count: int) -> None:
        with self.lock:
            self.room_user_count = count

    def record_rtt(self, rtt_ms: float) -> None:
        with self.lock:
            self.last_rtt_ms = round(rtt_ms, 2)
            self.rtt_samples.append(rtt_ms)

    def set_agent_state(
        self,
        nick: str,
        state: str,
        last_ms: Optional[float] = None,
        last_text: Optional[str] = None,
        actual_model: Optional[str] = None,
        inc_reply: bool = False,
    ) -> None:
        with self.lock:
            a = self.agents.get(nick)
            if not a:
                return
            a["state"] = state
            if last_ms is not None:
                a["last_ms"] = round(last_ms, 0)
            if last_text is not None:
                a["last_text"] = last_text
            if actual_model is not None:
                a["actual_model"] = actual_model
            if inc_reply:
                a["replies"] += 1
                self.total_bot_replies += 1

    def add_chat_event(self, speaker: str, text: str, is_bot: bool, model_tag: str = "", ms: Optional[float] = None) -> None:
        with self.lock:
            if not model_tag and speaker in self.agents:
                model_tag = str(self.agents[speaker]["actual_model"])
            if ms is None and speaker in self.agents:
                ms = self.agents[speaker]["last_ms"]
            self.chat_feed.append(
                {
                    "time": time.strftime("%H:%M:%S"),
                    "speaker": speaker,
                    "text": text,
                    "is_bot": is_bot,
                    "color": self.agents[speaker]["color"] if speaker in self.agents else "#E2E8F0",
                    "model_tag": model_tag if is_bot else "",
                    "ms": round(ms, 0) if (is_bot and ms is not None) else None,
                }
            )

    def _read_trace_stats(self, pid: Optional[int]) -> Dict[str, Any]:
        if not pid or not self.trace_dir.is_dir():
            return {"events": 0, "trace_kb": 0.0, "log_count": 0, "warn_count": 0, "recent_logs": []}
        traces = sorted(self.trace_dir.glob(f"*-{pid}.cvtrace"), key=lambda p: p.stat().st_mtime)
        logs = sorted(self.trace_dir.glob(f"*-{pid}.cvlog"), key=lambda p: p.stat().st_mtime)
        events = 0
        trace_kb = 0.0
        if traces:
            sz = traces[-1].stat().st_size
            trace_kb = round(sz / 1024.0, 1)
            events = max(0, (sz - 64) // 256)
        log_count = 0
        warn_count = 0
        recent_logs: List[Dict[str, Any]] = []
        if logs:
            try:
                lines = [ln for ln in logs[-1].read_text(encoding="utf-8", errors="replace").splitlines() if ln.strip()]
                log_count = len(lines)
                parsed = []
                for ln in lines[-30:]:
                    try:
                        parsed.append(json.loads(ln))
                    except Exception:
                        pass
                warn_count = sum(1 for x in parsed if str(x.get("level", "")).upper() in ("WARNING", "ERROR", "FATAL"))
                for x in parsed[-10:]:
                    ts_ms = int(x.get("time", 0))
                    ts_str = time.strftime("%H:%M:%S", time.localtime(ts_ms / 1000.0)) if ts_ms > 0 else "--:--:--"
                    recent_logs.append(
                        {
                            "time": ts_str,
                            "level": x.get("level", "INFO"),
                            "traceSeq": x.get("traceSeq", 0),
                            "msg": x.get("msg", ""),
                        }
                    )
            except Exception:
                pass
        return {
            "events": events,
            "trace_kb": trace_kb,
            "log_count": log_count,
            "warn_count": warn_count,
            "recent_logs": recent_logs,
        }

    def tick_sample(self) -> None:
        proc = self.proc_mon.sample()
        tstats = self._read_trace_stats(proc.get("pid"))
        now = time.monotonic()
        with self.lock:
            self.proc_stats = proc
            self.trace_stats = tstats
            cutoff = now - 2.0
            while self._pkt_timestamps and self._pkt_timestamps[0] < cutoff:
                self._pkt_timestamps.popleft()
            pkt_rate = round(len(self._pkt_timestamps) / 2.0, 1)
            self.series.append(
                {
                    "t": time.strftime("%H:%M:%S"),
                    "rtt": self.last_rtt_ms,
                    "pkt_s": pkt_rate,
                    "users": self.room_user_count,
                    "rss": proc.get("rss_mb", 0.0),
                    "events": tstats["events"],
                }
            )

    def set_squad_counts(self, per_agent_active: Dict[str, int], target_per_ai: int, deploying: bool) -> None:
        with self.lock:
            self.squad_target_per_ai = target_per_ai
            self.squad_deploying = deploying
            total = 0
            for nick, a in self.agents.items():
                cnt = int(per_agent_active.get(nick, 0))
                a["squad_active"] = cnt
                a["squad_target"] = target_per_ai
                total += cnt
            self.squad_total_active = total

    def add_squad_rx(self, nbytes: int, npackets: int = 1) -> None:
        with self.lock:
            self.squad_rx_bytes += nbytes
            self.squad_rx_packets += npackets

    def snapshot(self) -> Dict[str, Any]:
        now = time.monotonic()
        with self.lock:
            cutoff = now - 2.0
            pkt_rate = round(sum(1 for ts in self._pkt_timestamps if ts >= cutoff) / 2.0, 1)
            rtts = sorted(self.rtt_samples)
            p50 = round(rtts[len(rtts) // 2], 2) if rtts else 0.0
            p95 = round(rtts[min(len(rtts) - 1, int(math.ceil(len(rtts) * 0.95)) - 1)], 2) if rtts else 0.0
            return {
                "server": f"{self.host}:{self.port}",
                "uptime_s": int(time.time() - self.started_at),
                "room_users": self.room_user_count,
                "total_packets": self.total_packets,
                "total_chats": self.total_chats,
                "total_bot_replies": self.total_bot_replies,
                "pkt_per_sec": pkt_rate,
                "rtt_ms": {"latest": self.last_rtt_ms, "p50": p50, "p95": p95},
                "proc": dict(self.proc_stats),
                "trace": dict(self.trace_stats),
                "agents": list(self.agents.values()),
                "squad": {
                    "active": self.squad_total_active,
                    "target_per_ai": self.squad_target_per_ai,
                    "deploying": self.squad_deploying,
                    "rx_mb": round(self.squad_rx_bytes / (1024.0 * 1024.0), 2),
                    "rx_packets": self.squad_rx_packets,
                },
                "models": [{"key": m.key, "label": m.label, "short_tag": m.short_tag} for m in self.catalog.values()],
                "chat_feed": list(self.chat_feed)[-35:],
                "series": list(self.series),
                "demo_status": self.demo_status,
                "live_mode": self.live_mode,
                "stress_mode": self.stress_mode,
                "stress_sent": self.stress_sent,
                "load_running": self.load_running,
                "last_load_summary": self.last_load_summary,
            }


# ---- 개별 AI Agent TCP 세션 ----

class ChatAgentSession:
    def __init__(self, persona: AgentPersona, host: str, port: int, telemetry: TelemetryStore) -> None:
        self.persona = persona
        self.host = host
        self.port = port
        self.telemetry = telemetry
        self.reader: Optional[asyncio.StreamReader] = None
        self.writer: Optional[asyncio.StreamWriter] = None
        self._send_lock = asyncio.Lock()
        self.logged_in = False
        self._rtt_sent_at: Optional[float] = None

    async def connect_and_login(self) -> None:
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)
        self.writer.write(cs_login(self.persona.nickname))
        await self.writer.drain()

        while True:
            pkt = await self.read_packet()
            if pkt is None:
                raise ConnectionError(f"[{self.persona.nickname}] 로그인 응답 전 서버 연결 종료")
            ptype, body = pkt
            if ptype == SC_LOGIN_RESULT:
                res, cnt = parse_sc_login_result(body)
                if res != LOGIN_SUCCESS:
                    raise RuntimeError(f"[{self.persona.nickname}] 로그인 거절 (code={res})")
                self.logged_in = True
                self.telemetry.set_room_users(cnt)
                self.telemetry.set_agent_state(self.persona.nickname, "idle")
                mk = self.telemetry.get_agent_model_key(self.persona.nickname)
                print(f"[JOIN] {self.persona.nickname} ({mk}) 로그인 완료 (현재 방 인원: {cnt}명)", flush=True)
                return

    async def send_chat(self, text: str) -> str:
        if not self.writer or self.writer.is_closing():
            return ""
        safe = sanitize_chat_reply(text)
        if not safe:
            return ""
        async with self._send_lock:
            self.writer.write(cs_chat(safe))
            await self.writer.drain()
        return safe

    async def send_rtt_probe(self) -> None:
        if not self.writer or self.writer.is_closing() or not self.logged_in:
            return
        if self._rtt_sent_at is not None and (time.perf_counter() - self._rtt_sent_at) < 2.0:
            return
        self._rtt_sent_at = time.perf_counter()
        async with self._send_lock:
            self.writer.write(cs_room_users(0))
            await self.writer.drain()

    def on_room_users_response(self, total: int) -> None:
        self.telemetry.set_room_users(total)
        if self._rtt_sent_at is not None:
            rtt_ms = (time.perf_counter() - self._rtt_sent_at) * 1000.0
            self._rtt_sent_at = None
            self.telemetry.record_rtt(rtt_ms)

    async def read_packet(self) -> Optional[Tuple[int, bytes]]:
        assert self.reader is not None
        try:
            hdr = await self.reader.readexactly(PACKET_HEADER_SIZE)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            return None
        ptype, size = struct.unpack("<HH", hdr)
        if size > PACKET_MAX_BODY_SIZE:
            raise ValueError(f"Oversized server frame: {size}")
        if size == 0:
            return ptype, b""
        try:
            body = await self.reader.readexactly(size)
        except (asyncio.IncompleteReadError, ConnectionError, OSError):
            return None
        return ptype, body

    async def close(self) -> None:
        self.logged_in = False
        self.telemetry.set_agent_state(self.persona.nickname, "offline")
        if self.writer is not None:
            try:
                self.writer.close()
                await self.writer.wait_closed()
            except Exception:
                pass
            self.writer = None


# ---- 800명 AI 군단 매니저 (AI 모델당 200명 실제 TCP/Protobuf 세션 유지·드레인·발언 위임) ----

class SquadMember:
    __slots__ = ("nickname", "leader_nick", "reader", "writer", "drain_task", "alive")

    def __init__(self, nickname: str, leader_nick: str, reader: asyncio.StreamReader, writer: asyncio.StreamWriter) -> None:
        self.nickname = nickname
        self.leader_nick = leader_nick
        self.reader = reader
        self.writer = writer
        self.drain_task: Optional[asyncio.Task[None]] = None
        self.alive = True


class SquadSwarmManager:
    def __init__(self, host: str, port: int, personas: Sequence[AgentPersona], telemetry: TelemetryStore) -> None:
        self.host = host
        self.port = port
        self.personas = list(personas)
        self.telemetry = telemetry
        self.squads: Dict[str, List[SquadMember]] = {p.nickname: [] for p in personas}
        self.prefixes: Tuple[str, ...] = tuple(p.squad_prefix for p in personas)
        self._deploy_task: Optional[asyncio.Task[None]] = None
        self._active = False
        self._target_per_ai = 0

    def is_squad_nickname(self, nick: str) -> bool:
        return any(nick.startswith(pref) and len(nick) == len(pref) + 3 and nick[len(pref):].isdigit() for pref in self.prefixes)

    def squad_leader_for_nick(self, nick: str) -> Optional[str]:
        for p in self.personas:
            if nick.startswith(p.squad_prefix) and nick[len(p.squad_prefix):].isdigit():
                return p.nickname
        return None

    def _sync_counts(self, deploying: bool) -> None:
        counts = {nick: sum(1 for m in mems if m.alive) for nick, mems in self.squads.items()}
        self.telemetry.set_squad_counts(counts, self._target_per_ai, deploying)

    async def _drain_member(self, member: SquadMember) -> None:
        try:
            while member.alive:
                chunk = await member.reader.read(8192)
                if not chunk:
                    break
                self.telemetry.add_squad_rx(len(chunk), 1)
        except (asyncio.CancelledError, ConnectionError, OSError):
            pass
        finally:
            member.alive = False

    async def deploy(self, per_ai: int = 200, total_override: Optional[int] = None) -> Dict[str, Any]:
        await self.withdraw()
        self._active = True
        n_ai = max(1, len(self.personas))
        if total_override is not None and total_override > 0:
            base = total_override // n_ai
            rem = total_override % n_ai
            counts_by_persona = [base + (1 if idx < rem else 0) for idx in range(n_ai)]
            self._target_per_ai = max(counts_by_persona)
            total_target = sum(counts_by_persona)
        else:
            counts_by_persona = [per_ai for _ in range(n_ai)]
            self._target_per_ai = per_ai
            total_target = per_ai * n_ai
        self._sync_counts(deploying=True)
        self.telemetry.demo_status = f"🚀 {total_target}명 군단 투입 중 (AI {n_ai}인 분담 · 지속 유지)..."
        print(f"[SWARM] 총 {total_target}명 동시 접속 시작 (분담={counts_by_persona})", flush=True)

        t0 = time.perf_counter()
        sem = asyncio.Semaphore(64)

        async def connect_one(persona: AgentPersona, idx: int) -> Optional[SquadMember]:
            if not self._active:
                return None
            nick = f"{persona.squad_prefix}{idx:03d}"
            async with sem:
                if not self._active:
                    return None
                try:
                    r, w = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), timeout=5.0)
                    w.write(cs_login(nick))
                    await w.drain()
                    hdr = await asyncio.wait_for(r.readexactly(PACKET_HEADER_SIZE), timeout=5.0)
                    ptype, sz = struct.unpack("<HH", hdr)
                    body = await asyncio.wait_for(r.readexactly(sz), timeout=5.0) if sz > 0 else b""
                    if ptype == SC_LOGIN_RESULT:
                        res, _ = parse_sc_login_result(body)
                        if res != LOGIN_SUCCESS:
                            w.close()
                            return None
                    mem = SquadMember(nick, persona.nickname, r, w)
                    mem.drain_task = asyncio.create_task(self._drain_member(mem))
                    return mem
                except Exception:
                    return None

        # 웨이브(배치) 단위로 투입해 HUD 진행률과 서버 수락이 실시간으로 매끄럽게 보이도록
        wave = 40
        max_per = max(counts_by_persona) if counts_by_persona else 0
        for start in range(1, max_per + 1, wave):
            if not self._active:
                break
            end = min(max_per + 1, start + wave)
            coros = [
                connect_one(p, idx)
                for p, cnt in zip(self.personas, counts_by_persona)
                for idx in range(start, min(cnt + 1, end))
            ]
            res_list = await asyncio.gather(*coros)
            for mem in res_list:
                if mem is not None:
                    self.squads[mem.leader_nick].append(mem)
            self._sync_counts(deploying=True)

        elapsed_ms = (time.perf_counter() - t0) * 1000.0
        self._sync_counts(deploying=False)
        active_total = sum(sum(1 for m in ms if m.alive) for ms in self.squads.values())
        self.telemetry.demo_status = (
            f"✅ {active_total}명 군단 상시 접속 완료 ({elapsed_ms:.0f}ms) — 실제 군단원 소켓으로 지속 대화 진행 중"
        )
        self.telemetry.add_chat_event(
            "군단배치",
            f"🚀 {active_total}/{total_target}명 상시 접속 완료 ({elapsed_ms:.0f}ms) · 이제 군단원들이 실시간으로 대화를 이어갑니다",
            is_bot=True,
            model_tag=f"Swarm-{active_total}",
            ms=round(elapsed_ms, 0),
        )
        print(f"[SWARM_READY] {active_total}/{total_target}명 배치 완료 ({elapsed_ms:.0f}ms)", flush=True)
        return {"active": active_total, "target": total_target, "elapsed_ms": round(elapsed_ms, 1)}

    async def send_as_random_member(self, leader_nick: str, text: str) -> Optional[str]:
        mems = [m for m in self.squads.get(leader_nick, []) if m.alive and not m.writer.is_closing()]
        if not mems:
            return None
        chosen = random.choice(mems)
        safe = sanitize_chat_reply(text)
        if not safe:
            return None
        try:
            chosen.writer.write(cs_chat(safe))
            await chosen.writer.drain()
            return chosen.nickname
        except Exception:
            chosen.alive = False
            return None

    STRESS_LINES = (
        "지금 {n}명 몰렸는데 렉 1도 없네 ㄷㄷ",
        "IOCP 워커 4개로 {n}명 버티는 거 실화냐ㅋㅋ",
        "레이드 1파티 힐러 급구!! 바로 출발합니다",
        "방금 패킷 RTT {rtt}ms 찍힘 쾌적하네",
        "서버 메모리 {mem}MB밖에 안 먹는 거 맞음?",
        "채팅 올라가는 속도 미쳤다 유실 체크 중!",
        "아키님 스트랜드 락 경합 전혀 안 느껴지는데요?",
        "길드원 모집합니다 접속률 좋은 분 환영~",
        "보스 젠 30초 전!! 다들 집결하세요",
        "스캐터 개더 배칭 덕분인가 브로드캐스트 엄청 부드럽네",
        "지금 초당 패킷 그래프 춤추는 거 봐라ㅋㅋ",
        "QA팀 경계값 테스트 중인가요 정상 채팅은 멀쩡함!",
        "파티 구함 딜러2 힐러1 탱커1 ㄱㄱ",
        "와 {n}명 동시 폭주인데 클라 안 튕기네",
        "공유 불변 패킷 버퍼라 복사 부하가 안 튀나 보다",
        "여기가 그 전설의 C++20 MultiServer 채팅방인가요?",
        "거래소 시세 보신 분? 강화석 가격 엄청 올랐네",
        "방금 닉네임 페이징 넘겨봤는데 20명씩 깔끔하게 뜸",
        "힐러님 버프 좀요 ㅠ 앞라인 녹고 있어요",
        "로그 I/O 밀리나 확인 중인데 경고 0건이네!",
        "200자 한도 체크하면서 빠르게 연타 테스트 중!",
        "다들 채팅 너무 빨라서 눈이 못 따라가겠네ㅋㅋ",
        "부하 폭주 중에도 내 채팅 바로 찍히나 테스트~",
        "백프레셔 1MiB 방어선 작동 확인! 정상 유저 무영향",
        "기획님 다음 패치에 귓속말이랑 이모티콘도 넣어주세요!",
        "지금 브로드캐스트 초당 수만 건 날아다니는 중ㅋㅋ",
        "드레인 루프 돌아가니까 800소켓 다 살아있네",
        "오 여기 분위기 완전 대형 MMORPG 마을 광장 같다",
        "서버 CPU 점유율 보면서 스트레스 테스트 감상 중",
        "누가 보스 어그로 끌었냐 ㅋㅋㅋ 빨리 모여",
        "프로토버프 직렬화라 패킷 크기 진짜 가볍다",
        "실시간 트레이스 이벤트 수십만 건 쌓이는 중!",
    )

    def start_stress_storm(self, rate_per_sec: float = 28.0) -> None:
        self.stop_stress_storm()
        self.telemetry.stress_mode = True
        workers = 4
        per_worker_interval = max(0.04, workers / max(1.0, rate_per_sec))

        async def _storm_worker(worker_idx: int) -> None:
            try:
                while self.telemetry.stress_mode and self._active:
                    all_alive = [
                        m for ms in self.squads.values() for m in ms
                        if m.alive and not m.writer.is_closing()
                    ]
                    if not all_alive:
                        await asyncio.sleep(0.25)
                        continue
                    chosen = random.choice(all_alive)
                    n_users = max(self.telemetry.room_user_count, len(all_alive))
                    rtt = round(self.telemetry.last_rtt_ms, 2)
                    mem = round(float(self.telemetry.proc_stats.get("rss_mb", 0.0)), 1)
                    tpl = random.choice(self.STRESS_LINES)
                    msg = tpl.format(n=n_users, rtt=rtt, mem=mem)
                    try:
                        chosen.writer.write(cs_chat(msg))
                        await chosen.writer.drain()
                        self.telemetry.stress_sent += 1
                    except Exception:
                        chosen.alive = False
                    jitter = random.uniform(0.65, 1.35)
                    await asyncio.sleep(per_worker_interval * jitter)
            except asyncio.CancelledError:
                return

        self._storm_tasks: List[asyncio.Task[None]] = [
            asyncio.create_task(_storm_worker(i)) for i in range(workers)
        ]

    def stop_stress_storm(self) -> None:
        self.telemetry.stress_mode = False
        for t in getattr(self, "_storm_tasks", []):
            if not t.done():
                t.cancel()
        self._storm_tasks = []

    async def withdraw(self) -> None:
        self.stop_stress_storm()
        self._active = False
        all_members = [m for ms in self.squads.values() for m in ms]
        for nick in self.squads:
            self.squads[nick] = []
        for m in all_members:
            m.alive = False
            if m.drain_task and not m.drain_task.done():
                m.drain_task.cancel()
            try:
                m.writer.close()
            except Exception:
                pass
        if all_members:
            await asyncio.sleep(0.15)
        self._target_per_ai = 0
        self._sync_counts(deploying=False)
        if all_members:
            self.telemetry.demo_status = f"⏹ {len(all_members)}명 군단 철수 완료 (리더 AI 4인 대기)"
            print(f"[SWARM_STOP] {len(all_members)}명 철수 완료", flush=True)


# ---- 중앙 대화 오케스트레이터 + 시연 액션 ----

class DialogueOrchestrator:
    def __init__(
        self,
        sessions: Sequence[ChatAgentSession],
        router: MultiModelRouter,
        telemetry: TelemetryStore,
        swarm: SquadSwarmManager,
        host: str,
        port: int,
        banter_turns: int,
        turn_pause: float,
        stop_event: asyncio.Event,
    ) -> None:
        self.sessions: Dict[str, ChatAgentSession] = {s.persona.nickname: s for s in sessions}
        self.personas: List[AgentPersona] = [s.persona for s in sessions]
        self.bot_nicknames: Set[str] = set(self.sessions.keys())
        self.router = router
        self.telemetry = telemetry
        self.swarm = swarm
        self.host = host
        self.port = port
        self.banter_turns = max(0, banter_turns)
        self.turn_pause = max(0.2, turn_pause)
        self.stop_event = stop_event

        self.history: Deque[Tuple[str, str]] = deque(maxlen=32)
        self._generation = 0
        self._dialogue_task: Optional[asyncio.Task[None]] = None
        self._last_speaker: Optional[str] = None
        self._load_task: Optional[asyncio.Task[None]] = None

    def _find_mentioned(self, text: str) -> List[str]:
        lowered = text.lower()
        hits: List[str] = []
        for p in self.personas:
            if p.nickname.lower() in lowered or any(a.lower() in lowered for a in p.aliases):
                hits.append(p.nickname)
        return hits

    def _cancel_current_dialogue(self) -> int:
        self._generation += 1
        if self._dialogue_task and not self._dialogue_task.done():
            self._dialogue_task.cancel()
        for p in self.personas:
            if self.sessions[p.nickname].logged_in:
                self.telemetry.set_agent_state(p.nickname, "idle")
        return self._generation

    def trigger_debate(self, topic: str, turns: int = 6, initiator: str = "시연 컨트롤러") -> None:
        gen = self._cancel_current_dialogue()
        self.telemetry.demo_status = f"🔥 이종 멀티모델 AI 자율 토론 ({turns}턴): {topic}"
        # 첫 4턴은 4명 전원이 한 번씩 발언하고 나머지는 랜덤 티키타카
        base = [p.nickname for p in self.personas]
        order = base[:turns]
        if turns > len(base):
            order.extend(self._build_banter_order(count=turns - len(base), avoid_first=order[-1]))
        self._dialogue_task = asyncio.create_task(
            self._run_sequence(
                gen,
                order,
                mode="debate",
                topic_note=f"토론 주제 '{topic}'에 대해 각자의 역할로 앞선 동료 발언을 직접 받아치며(반박·확장·중재) 생생하게 토론하세요.",
            )
        )

    def trigger_continue(self, turns: int = 4) -> None:
        gen = self._cancel_current_dialogue()
        self.telemetry.demo_status = f"💬 AI 에이전트 자율 대화 이어가기 ({turns}턴)"
        order = self._build_banter_order(count=turns, avoid_first=self._last_speaker)
        self._dialogue_task = asyncio.create_task(
            self._run_sequence(
                gen,
                order,
                mode="banter",
                topic_note="방금까지의 대화 흐름을 이어받아 동료 에이전트끼리 자연스럽게 티키타카 대화를 이어가세요.",
            )
        )

    LIVE_TOPICS = (
        "C++ 채팅 서버에 다음에 붙일 가장 재미있는 기능",
        "동시 접속자 수백 명이 몰릴 때 체감 지연을 줄이는 현실적인 아이디어",
        "실시간 채팅방에서 가장 까다로운 예외 상황과 보안 방어 전략",
        "게임 서버 개발팀이 야근을 줄이기 위해 꼭 도입해야 할 자동화 도구",
        "레이드·길드 채팅으로 확장할 때 서버와 클라이언트가 나눠 맡아야 할 역할",
    )

    def trigger_live_start(self, topic: str = "") -> None:
        gen = self._cancel_current_dialogue()
        self.telemetry.live_mode = True
        chosen = topic.strip() or self.LIVE_TOPICS[0]
        self.telemetry.demo_status = f"🔁 이종 멀티모델 지속 라이브 대화 진행 중: {chosen}"
        self._dialogue_task = asyncio.create_task(self._run_continuous_live(gen, chosen))

    def trigger_stop(self) -> None:
        self.telemetry.live_mode = False
        self.swarm.stop_stress_storm()
        self._cancel_current_dialogue()
        self.telemetry.demo_status = "⏸ AI 대화·폭주 채팅 일시 중단 (버튼이나 채팅 입력으로 언제든 재개)"
        print("[CTRL] 에이전트 대화 및 폭주 채팅을 중단했습니다.", flush=True)

    def trigger_stress_toggle(self, topic: str = "") -> None:
        if self.telemetry.stress_mode:
            self.swarm.stop_stress_storm()
            self.telemetry.demo_status = "⏹ 난잡 폭주 채팅 중지 (군단 접속·AI 대화 유지)"
            return
        if self.telemetry.squad_total_active <= 0:
            self.trigger_swarm_start(per_ai=200, total_override=None, auto_live=True, stress_storm=True, topic=topic)
            return
        rate = 28.0 if self.telemetry.squad_total_active >= 200 else 12.0
        self.swarm.start_stress_storm(rate_per_sec=rate)
        self.telemetry.demo_status = f"🔥 {self.telemetry.squad_total_active}명 난잡 폭주 채팅 가동 중 (~{int(rate)} msg/s × {self.telemetry.room_user_count}명 브로드캐스트)"

    def trigger_load_burst(self, client_count: int = 50, chat_burst: int = 40) -> None:
        if self._load_task and not self._load_task.done():
            return
        self._load_task = asyncio.create_task(self._run_load_burst(client_count, chat_burst))

    def trigger_warning_probe(self) -> None:
        asyncio.create_task(self._run_warning_probe())

    def trigger_swarm_start(
        self,
        per_ai: int = 200,
        total_override: Optional[int] = None,
        auto_live: bool = True,
        stress_storm: bool = False,
        topic: str = "",
    ) -> None:
        async def _deploy_and_talk() -> None:
            res = await self.swarm.deploy(per_ai=per_ai, total_override=total_override)
            if self.stop_event.is_set():
                return
            if stress_storm and res.get("active", 0) > 0:
                rate = 28.0 if res["active"] >= 200 else 10.0
                self.swarm.start_stress_storm(rate_per_sec=rate)
            if auto_live:
                if not self.telemetry.live_mode or not self._dialogue_task or self._dialogue_task.done():
                    live_topic = topic.strip() or f"{self.telemetry.room_user_count}명 동시 접속 서버 실시간 성능 체감과 기능 아이디어"
                    self.trigger_live_start(live_topic)

        asyncio.create_task(_deploy_and_talk())

    def trigger_swarm_stop(self) -> None:
        asyncio.create_task(self.swarm.withdraw())

    async def _run_load_burst(self, client_count: int, chat_burst: int) -> None:
        self.telemetry.load_running = True
        clients: List[Tuple[asyncio.StreamReader, asyncio.StreamWriter, asyncio.Task[None]]] = []
        rejected_full = 0

        async def drain_reader(reader: asyncio.StreamReader) -> None:
            try:
                while True:
                    chunk = await reader.read(8192)
                    if not chunk:
                        break
            except (asyncio.CancelledError, ConnectionError, OSError):
                pass

        try:
            self.telemetry.demo_status = f"⚡ {client_count}명 동시 접속 + {chat_burst}건 채팅 부하 투입 중..."
            self.telemetry.add_chat_event(
                "부하시연",
                f"⚡ {client_count}명 동시 접속 + {chat_burst}건 채팅 버스트 시작 (현재 방 {self.telemetry.room_user_count}명)",
                is_bot=True,
                model_tag="LoadBurst",
            )
            print(f"[LOAD] {client_count}명 동시 접속 + {chat_burst}건 부하 시작", flush=True)
            t0 = time.perf_counter()

            async def join_one(i: int) -> Tuple[Optional[Tuple[asyncio.StreamReader, asyncio.StreamWriter, asyncio.Task[None]]], bool]:
                try:
                    r, w = await asyncio.wait_for(asyncio.open_connection(self.host, self.port), timeout=4.0)
                    w.write(cs_login(f"부하봇{i:03d}"))
                    await w.drain()
                    hdr = await asyncio.wait_for(r.readexactly(PACKET_HEADER_SIZE), timeout=4.0)
                    ptype, sz = struct.unpack("<HH", hdr)
                    body = await asyncio.wait_for(r.readexactly(sz), timeout=4.0) if sz > 0 else b""
                    if ptype == SC_LOGIN_RESULT:
                        res, _ = parse_sc_login_result(body)
                        if res != LOGIN_SUCCESS:
                            w.close()
                            return (None, True)
                    dt = asyncio.create_task(drain_reader(r))
                    return ((r, w, dt), False)
                except Exception:
                    return (None, False)

            results = await asyncio.gather(*(join_one(i) for i in range(1, client_count + 1)))
            for item, was_rejected in results:
                if item is not None:
                    clients.append(item)
                elif was_rejected:
                    rejected_full += 1
            login_ms = (time.perf_counter() - t0) * 1000.0

            if not clients:
                reason = f"방 정원(1,000명) 도달로 {rejected_full}건 거부" if rejected_full else "서버 응답 없음"
                self.telemetry.demo_status = f"⚠️ 부하 시연 실패: 0/{client_count}명 로그인 ({reason})"
                self.telemetry.add_chat_event("부하시연", self.telemetry.demo_status, is_bot=True, model_tag="LoadBurst")
                return

            await asyncio.sleep(1.5)
            room_peak = max(self.telemetry.room_user_count, len(clients))

            t_chat = time.perf_counter()
            for i in range(chat_burst):
                _, w, _ = clients[i % len(clients)]
                w.write(cs_chat(f"[실시간부하측정 #{i+1:02d}] 동시접속 {room_peak}명 스트레스 브로드캐스트"))
                await w.drain()
                if i % 8 == 7:
                    await asyncio.sleep(0.05)
            chat_ms = (time.perf_counter() - t_chat) * 1000.0

            await asyncio.sleep(1.8)
            delivered_est = room_peak * chat_burst
            summary = {
                "clients": len(clients),
                "rejected_full": rejected_full,
                "room_peak": room_peak,
                "login_ms": round(login_ms, 1),
                "chat_burst": chat_burst,
                "broadcast_delivered_est": delivered_est,
                "chat_ms": round(chat_ms, 1),
            }
            self.telemetry.last_load_summary = summary
            self.telemetry.demo_status = (
                f"✅ 부하 시연 완료: {len(clients)}/{client_count}명 로그인 {login_ms:.1f}ms · "
                f"{chat_burst}건 송신 → {room_peak}명 방 총 {delivered_est:,}건 브로드캐스트 ({chat_ms:.1f}ms)"
            )
            self.telemetry.add_chat_event(
                "부하시연",
                f"✅ {len(clients)}명 로그인 {login_ms:.1f}ms · {chat_burst}건 → {room_peak}명 방 {delivered_est:,}건 브로드캐스트 ({chat_ms:.1f}ms)",
                is_bot=True,
                model_tag="LoadBurst",
                ms=round(chat_ms, 1),
            )
            print(f"[LOAD_OK] {summary}", flush=True)
        except Exception as exc:
            self.telemetry.demo_status = f"⚠️ 부하 시연 오류: {exc}"
            self.telemetry.add_chat_event("부하시연", self.telemetry.demo_status, is_bot=True, model_tag="LoadBurst")
        finally:
            for _, w, dt in clients:
                if not dt.done():
                    dt.cancel()
                try:
                    w.close()
                except Exception:
                    pass
            self.telemetry.load_running = False

    async def _run_warning_probe(self) -> None:
        self.telemetry.demo_status = "🛡️ 보안/경고 정책 4종 실시간 주입 중 (.cvlog 확인)..."
        try:
            _, w = await asyncio.open_connection(self.host, self.port)
            w.write(cs_login("보안점검봇"))
            await w.drain()
            await asyncio.sleep(0.15)
            w.write(cs_chat(""))
            w.write(cs_chat("가" * 201))
            await w.drain()
            await asyncio.sleep(0.2)
            w.close()

            _, w2 = await asyncio.open_connection(self.host, self.port)
            w2.write(frame_packet(999, b"\x08\x01"))
            await w2.drain()
            await asyncio.sleep(0.15)
            w2.close()

            _, w3 = await asyncio.open_connection(self.host, self.port)
            w3.write(struct.pack("<HH", CS_CHAT, PACKET_MAX_BODY_SIZE + 1))
            await w3.drain()
            await asyncio.sleep(0.15)
            w3.close()

            self.telemetry.demo_status = "✅ 경고·차단 정책 4종 주입 완료 (.cvlog 실시간 경고 확인, 정상 채팅 무영향)"
            print("[WARN_PROBE] 4종 경고/차단 주입 완료", flush=True)
        except Exception as exc:
            self.telemetry.demo_status = f"경고 시험 실패: {exc}"

    def on_room_chat(self, speaker: str, text: str) -> None:
        is_squad = self.swarm.is_squad_nickname(speaker)
        is_bot = speaker in self.bot_nicknames or is_squad or speaker.startswith("부하봇") or speaker == "보안점검봇"
        self.telemetry.record_packet(is_chat=True)
        if not speaker.startswith("부하봇"):
            self.history.append((speaker, text))
            self._last_speaker = speaker
            leader = self.swarm.squad_leader_for_nick(speaker) if is_squad else speaker
            mtag = ""
            lms = None
            if leader and leader in self.telemetry.agents:
                mtag = str(self.telemetry.agents[leader]["actual_model"])
                lms = self.telemetry.agents[leader]["last_ms"]
            self.telemetry.add_chat_event(speaker, text, is_bot=is_bot, model_tag=mtag, ms=lms)

        if is_bot:
            return

        print(f"[USER] {speaker}: {text}", flush=True)
        stripped = text.strip()

        if stripped in ("/멈춰", "/조용", "/stop", "/pause"):
            self.trigger_stop()
            return

        if stripped in ("/계속", "/대화", "/more"):
            self.trigger_continue(turns=4)
            return

        if stripped.startswith("/토론"):
            topic = stripped[len("/토론"):].strip() or "C++ 고성능 채팅 서버에 다음에 붙일 최고의 기능"
            self.trigger_debate(topic, turns=6, initiator=speaker)
            return

        if stripped.startswith("/군단"):
            parts = stripped.split()
            per_ai = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 200
            per_ai = max(10, min(240, per_ai))
            self.trigger_swarm_start(per_ai=per_ai)
            return

        if stripped in ("/철수", "/군단종료"):
            self.trigger_swarm_stop()
            return

        if stripped.startswith("/부하"):
            parts = stripped.split()
            cnt = int(parts[1]) if len(parts) > 1 and parts[1].isdigit() else 50
            cnt = max(5, min(200, cnt))
            self.trigger_load_burst(client_count=cnt, chat_burst=40)
            return

        if stripped in ("/경고테스트", "/경고", "/보안"):
            self.trigger_warning_probe()
            return

        gen = self._cancel_current_dialogue()
        mentioned = self._find_mentioned(stripped)
        if mentioned:
            follow_count = min(2, self.banter_turns)
            others = [p.nickname for p in self.personas if p.nickname not in mentioned]
            random.shuffle(others)
            banter_order = others[:follow_count]
            self.telemetry.demo_status = f"💬 {', '.join(mentioned)} 호출 응답 + 동료 티키타카 진행 중"
            self._dialogue_task = asyncio.create_task(
                self._run_user_and_banter(gen, speaker, mentioned, banter_order)
            )
        else:
            primary = [p.nickname for p in self.personas]
            banter_order = self._build_banter_order(count=self.banter_turns, avoid_first=primary[-1])
            self.telemetry.demo_status = f"💬 {speaker}님 발언에 이종 모델 4인 응답 + 자율 대화 {len(banter_order)}턴 진행 중"
            self._dialogue_task = asyncio.create_task(
                self._run_user_and_banter(gen, speaker, primary, banter_order)
            )

    def _build_banter_order(self, count: int, avoid_first: Optional[str]) -> List[str]:
        if count <= 0 or not self.personas:
            return []
        names = [p.nickname for p in self.personas]
        order: List[str] = []
        prev = avoid_first
        for _ in range(count):
            candidates = [n for n in names if n != prev] or names
            pick = random.choice(candidates)
            order.append(pick)
            prev = pick
        return order

    async def _speak_once(self, gen: int, nick: str, instruction: str, is_banter: bool) -> bool:
        if gen != self._generation or self.stop_event.is_set():
            return False
        sess = self.sessions.get(nick)
        if not sess or not sess.logged_in:
            return False

        model_key = self.telemetry.get_agent_model_key(nick)
        self.telemetry.set_agent_state(nick, "thinking")
        snapshot = list(self.history)
        try:
            reply, used_prof, ms = await asyncio.to_thread(
                self.router.generate_sync, sess.persona, model_key, snapshot, instruction
            )
        except Exception as exc:
            self.telemetry.set_agent_state(nick, "idle")
            print(f"[WARN] {nick} 응답 실패: {exc}", flush=True)
            return False

        if gen != self._generation or self.stop_event.is_set():
            self.telemetry.set_agent_state(nick, "idle")
            return False

        sent = ""
        actual_speaker = nick
        # 800명 군단 배치 상태의 자율 티키타카 턴에서는 해당 AI가 관리하는 200명 실제 소켓 중 한 명으로 직접 발언 위임
        if is_banter and self.telemetry.squad_total_active > 0:
            delegated = await self.swarm.send_as_random_member(nick, reply)
            if delegated:
                sent = sanitize_chat_reply(reply)
                actual_speaker = delegated
        if not sent:
            sent = await sess.send_chat(reply)

        self.telemetry.set_agent_state(
            nick,
            "idle",
            last_ms=ms,
            last_text=f"[{actual_speaker}] {sent}" if actual_speaker != nick else sent,
            actual_model=used_prof.short_tag,
            inc_reply=bool(sent),
        )
        if sent:
            tag = "SQUAD" if actual_speaker != nick else ("BANTER" if is_banter else "REPLY")
            print(f"[{tag}] {actual_speaker} <- {nick} ({used_prof.short_tag}, {ms:.0f}ms) -> {sent}", flush=True)
            await asyncio.sleep(0.12)
            if not any(sp == actual_speaker and tx == sent for sp, tx in list(self.history)[-4:]):
                self.history.append((actual_speaker, sent))
            return True
        return False

    async def _run_user_and_banter(
        self,
        gen: int,
        human_speaker: str,
        primary_order: List[str],
        banter_order: List[str],
    ) -> None:
        try:
            for idx, nick in enumerate(primary_order):
                if gen != self._generation or self.stop_event.is_set():
                    return
                if idx == 0:
                    inst = f"{human_speaker}님의 말에 당신의 성격대로 먼저 답하세요."
                else:
                    inst = (
                        f"{human_speaker}님의 말에 답하면서, 바로 직전 동료 에이전트의 의견에도 "
                        "짧게 반응(공감·반박·보완)하며 대화를 이어가세요."
                    )
                await self._speak_once(gen, nick, inst, is_banter=False)
                if idx + 1 < len(primary_order):
                    await asyncio.sleep(min(0.6, self.turn_pause * 0.4))

            for nick in banter_order:
                if gen != self._generation or self.stop_event.is_set():
                    return
                await asyncio.sleep(self.turn_pause)
                if gen != self._generation or self.stop_event.is_set():
                    return
                last_bot = self.history[-1][0] if self.history else "동료"
                inst = (
                    f"방금 {last_bot}님이 한 말을 직접 콕 집어(이름을 불러도 좋음) "
                    "당신의 성격대로 반박하거나 농담·확장·중재하며 동료끼리 자연스럽게 대화를 주고받으세요."
                )
                await self._speak_once(gen, nick, inst, is_banter=True)
            if gen == self._generation:
                if self.telemetry.live_mode and not self.stop_event.is_set():
                    await self._run_continuous_live(gen, "방금 사용자님과 나눈 주제의 후속 토론")
                else:
                    self.telemetry.demo_status = "✅ 대화 라운드 완료 ('🔁 지속 대화 시작' 또는 채팅으로 이어갈 수 있어요)"
        except asyncio.CancelledError:
            return

    async def _run_continuous_live(self, gen: int, initial_topic: str) -> None:
        """이종 4개 LLM이 병렬로 대사를 생성해 0.45~0.85초 템포로 빠르게 주고받는 실시간 라이브 대화."""
        out_q: asyncio.Queue[Tuple[str, str, ModelProfile, float]] = asyncio.Queue(maxsize=12)
        topic_state = {"topic": initial_topic, "turns": 0, "topic_idx": 0}

        async def _model_producer(persona: AgentPersona, stagger_s: float) -> None:
            await asyncio.sleep(stagger_s)
            nick = persona.nickname
            sess = self.sessions.get(nick)
            while gen == self._generation and self.telemetry.live_mode and not self.stop_event.is_set():
                if not sess or not sess.logged_in:
                    await asyncio.sleep(0.5)
                    continue
                if out_q.qsize() >= 6:
                    await asyncio.sleep(0.35)
                    continue
                model_key = self.telemetry.get_agent_model_key(nick)
                self.telemetry.set_agent_state(nick, "thinking")
                snapshot = list(self.history)[-10:]
                last_sp = snapshot[-1][0] if snapshot else "동료"
                cur_topic = topic_state["topic"]
                inst = (
                    f"주제 '{cur_topic}'에서 방금 {last_sp}님의 말에 바로 끼어들어 "
                    "1~2문장으로 짧고 재치 있게 반박·확장·중재하세요."
                )
                try:
                    reply, used_prof, ms = await asyncio.to_thread(
                        self.router.generate_sync, persona, model_key, snapshot, inst
                    )
                    if gen == self._generation and self.telemetry.live_mode and reply:
                        await out_q.put((nick, reply, used_prof, ms))
                except Exception as exc:
                    print(f"[WARN] {nick} 병렬 생성 실패: {exc}", flush=True)
                finally:
                    self.telemetry.set_agent_state(nick, "idle")
                # 모델별 과도한 연속 호출 방지용 짧은 숨고르기
                await asyncio.sleep(random.uniform(0.6, 1.4))

        producers = [
            asyncio.create_task(_model_producer(p, idx * 0.25))
            for idx, p in enumerate(self.personas)
        ]
        try:
            while gen == self._generation and self.telemetry.live_mode and not self.stop_event.is_set():
                try:
                    nick, reply, used_prof, ms = await asyncio.wait_for(out_q.get(), timeout=1.0)
                except asyncio.TimeoutError:
                    continue
                if gen != self._generation or not self.telemetry.live_mode or self.stop_event.is_set():
                    break

                sess = self.sessions.get(nick)
                if not sess or not sess.logged_in:
                    continue
                sent = ""
                actual_speaker = nick
                if self.telemetry.squad_total_active > 0:
                    delegated = await self.swarm.send_as_random_member(nick, reply)
                    if delegated:
                        sent = sanitize_chat_reply(reply)
                        actual_speaker = delegated
                if not sent:
                    sent = await sess.send_chat(reply)

                self.telemetry.set_agent_state(
                    nick,
                    "idle",
                    last_ms=ms,
                    last_text=f"[{actual_speaker}] {sent}" if actual_speaker != nick else sent,
                    actual_model=used_prof.short_tag,
                    inc_reply=bool(sent),
                )
                if sent:
                    topic_state["turns"] += 1
                    t_cnt = topic_state["turns"]
                    if t_cnt % 16 == 0:
                        topic_state["topic_idx"] = (topic_state["topic_idx"] + 1) % len(self.LIVE_TOPICS)
                        topic_state["topic"] = self.LIVE_TOPICS[topic_state["topic_idx"]]
                    sq = self.telemetry.squad_total_active
                    if self.telemetry.stress_mode:
                        self.telemetry.demo_status = (
                            f"🔥 {sq}명 난잡 폭주 채팅({self.telemetry.stress_sent:,}건 송신) + "
                            f"이종 4모델 AI 병렬 발언({t_cnt}회) 동시 가동 중"
                        )
                    else:
                        sq_note = f" · 군단 {sq}명 소켓 가동" if sq > 0 else ""
                        self.telemetry.demo_status = f"🔁 이종 4모델 병렬 빠른 대화 진행 중 ({t_cnt}턴째{sq_note})"
                    if not any(sp == actual_speaker and tx == sent for sp, tx in list(self.history)[-4:]):
                        self.history.append((actual_speaker, sent))
                await asyncio.sleep(random.uniform(0.35, 0.75))
        except asyncio.CancelledError:
            return
        finally:
            for t in producers:
                if not t.done():
                    t.cancel()
            await asyncio.gather(*producers, return_exceptions=True)

    async def _run_sequence(self, gen: int, order: List[str], mode: str, topic_note: str) -> None:
        try:
            for idx, nick in enumerate(order):
                if gen != self._generation or self.stop_event.is_set():
                    return
                if idx > 0:
                    await asyncio.sleep(self.turn_pause)
                if gen != self._generation or self.stop_event.is_set():
                    return
                last_sp = self.history[-1][0] if self.history else "동료"
                inst = f"{topic_note} 특히 직전 발언자({last_sp})의 말을 직접 받아쳐서 대화를 이어가세요."
                await self._speak_once(gen, nick, inst, is_banter=(mode != "reply"))
            if gen == self._generation:
                self.telemetry.demo_status = "✅ 이종 멀티모델 자율 토론 라운드 완료"
        except asyncio.CancelledError:
            return


# ---- 실시간 계측 + 이종 모델 스위처 HUD ----

HUD_HTML = """<!doctype html>
<html lang="ko">
<head>
<meta charset="utf-8">
<title>MultiServer 실시간 서버 계측 & 이종 멀티모델 AI HUD</title>
<style>
  :root {
    --bg: #0b1120; --panel: #111a2e; --border: #233252; --text: #e2e8f0;
    --muted: #94a3b8; --accent: #38bdf8; --green: #4ade80; --amber: #fbbf24; --red: #f87171;
  }
  * { box-sizing: border-box; margin: 0; padding: 0; }
  body {
    background: var(--bg); color: var(--text);
    font-family: 'Malgun Gothic', 'Segoe UI', sans-serif;
    padding: 14px 16px; font-size: 13px;
  }
  .header {
    display: flex; justify-content: space-between; align-items: center;
    border-bottom: 1px solid var(--border); padding-bottom: 10px; margin-bottom: 12px;
  }
  .header h1 { font-size: 18px; color: #f8fafc; letter-spacing: -0.01em; }
  .badge {
    display: inline-flex; align-items: center; gap: 6px; padding: 4px 10px;
    border-radius: 999px; font-weight: 700; font-size: 12px;
    background: rgba(74, 222, 128, 0.14); color: var(--green); border: 1px solid rgba(74,222,128,0.35);
  }
  .badge.off { background: rgba(248,113,113,0.14); color: var(--red); border-color: rgba(248,113,113,0.35); }
  .dot { width: 8px; height: 8px; border-radius: 50%; background: currentColor; }
  .controls {
    display: flex; flex-wrap: wrap; gap: 8px; align-items: center;
    background: var(--panel); border: 1px solid var(--border); border-radius: 10px;
    padding: 10px 12px; margin-bottom: 12px;
  }
  .controls input {
    flex: 1; min-width: 200px; background: #0b1324; color: var(--text);
    border: 1px solid #33476e; border-radius: 7px; padding: 7px 10px; font-size: 13px;
  }
  .btn {
    border: 1px solid #33476e; background: #192847; color: #f1f5f9;
    padding: 7px 12px; border-radius: 7px; font-weight: 700; cursor: pointer; font-size: 12.5px;
  }
  .btn:hover { background: #233761; }
  .btn.primary { background: #0284c7; border-color: #38bdf8; }
  .btn.primary:hover { background: #0369a1; }
  .btn.warn { background: #9a3412; border-color: #fb923c; }
  .btn.danger { background: #7f1d1d; border-color: #f87171; }
  .status-line { width: 100%; font-size: 12px; color: #bae6fd; padding-top: 4px; overflow-wrap: anywhere; }
  .kpi-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(165px, 100%), 1fr)); gap: 10px; margin-bottom: 12px; }
  .kpi { background: var(--panel); border: 1px solid var(--border); border-radius: 10px; padding: 10px 12px; min-width: 0; }
  .kpi .k { font-size: 11px; color: var(--muted); margin-bottom: 4px; }
  .kpi .v { font-size: 19px; font-weight: 800; color: #f8fafc; font-variant-numeric: tabular-nums; }
  .kpi .s { font-size: 11px; color: #7dd3fc; margin-top: 3px; white-space: nowrap; overflow: hidden; text-overflow: ellipsis; }
  .main-grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(420px, 100%), 1fr)); gap: 12px; }
  .panel {
    background: var(--panel); border: 1px solid var(--border); border-radius: 10px;
    padding: 10px 12px; display: flex; flex-direction: column; gap: 8px; min-width: 0;
  }
  .panel h2 { font-size: 13.5px; color: #cbd5e1; display: flex; flex-wrap: wrap; justify-content: space-between; gap: 6px; }
  .agents { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(220px, 100%), 1fr)); gap: 8px; }
  .agent {
    background: #0c1426; border: 1px solid #22314f; border-radius: 8px; padding: 8px 10px;
    border-left: 4px solid var(--accent); display: flex; flex-direction: column; gap: 4px; min-width: 0;
  }
  .agent-top { display: flex; justify-content: space-between; align-items: center; }
  .agent-name { font-weight: 800; font-size: 13px; }
  .agent-state { font-size: 11px; padding: 2px 7px; border-radius: 999px; background: #1e293b; color: #cbd5e1; }
  .agent-state.thinking { background: rgba(251,191,36,0.2); color: var(--amber); font-weight: 700; }
  .agent-meta { font-size: 11px; color: var(--muted); overflow-wrap: anywhere; }
  .model-sel {
    width: 100%; background: #101c34; color: #bae6fd; border: 1px solid #2b4066;
    border-radius: 6px; padding: 3px 6px; font-size: 11px; font-weight: 700;
  }
  .agent-last { font-size: 11.5px; color: #e2e8f0; overflow-wrap: anywhere; }
  .charts { display: grid; grid-template-columns: repeat(auto-fit, minmax(min(190px, 100%), 1fr)); gap: 8px; }
  .chart-box { background: #0c1426; border: 1px solid #22314f; border-radius: 8px; padding: 8px; min-width: 0; }
  .chart-title { display: flex; justify-content: space-between; font-size: 11px; color: var(--muted); margin-bottom: 4px; }
  svg { width: 100%; height: 60px; display: block; }
  .feed {
    background: #09101f; border: 1px solid #1e2c48; border-radius: 8px; padding: 8px 10px;
    height: clamp(220px, 36vh, 380px); overflow-y: auto; display: flex; flex-direction: column; gap: 5px;
  }
  .msg { font-size: 12px; line-height: 1.42; overflow-wrap: anywhere; }
  .msg .tm { color: #64748b; font-size: 10.5px; margin-right: 4px; }
  .msg .sp { font-weight: 800; margin-right: 4px; }
  .msg .mtag {
    display: inline-block; font-size: 10px; padding: 1px 5px; border-radius: 4px;
    background: #172540; color: #93c5fd; margin-right: 5px; font-family: Consolas, monospace;
  }
  .log-list {
    background: #09101f; border: 1px solid #1e2c48; border-radius: 8px; padding: 8px 10px;
    height: clamp(110px, 18vh, 180px); overflow-y: auto; font-family: Consolas, monospace; font-size: 11.5px;
  }
  .log-row { margin-bottom: 4px; line-height: 1.35; overflow-wrap: anywhere; }
  .lvl-WARNING { color: var(--amber); font-weight: 700; }
  .lvl-ERROR, .lvl-FATAL { color: var(--red); font-weight: 700; }
  .lvl-INFO { color: #7dd3fc; }
</style>
</head>
<body>
  <div class="header" style="flex-wrap:wrap; gap:8px;">
    <div>
      <h1>MultiServer 실시간 서버 계측 &amp; 이종 멀티모델 AI 시연 HUD</h1>
      <div style="font-size:11.5px; color:#94a3b8; margin-top:2px;">
        ChangeViewer 통합 탭(<a href="http://127.0.0.1:8765/#aihud" style="color:#38bdf8">127.0.0.1:8765/#aihud</a>)과 동일 연동 &nbsp;·&nbsp; <strong>Ollama Qwen 14B + Antseed DeepSeek 4.1 (Flash · 1017 · Free)</strong>
      </div>
    </div>
    <div id="srvBadge" class="badge"><span class="dot"></span><span id="srvBadgeText">연결 확인 중...</span></div>
  </div>

  <div class="controls">
    <input id="topicInput" value="C++ 채팅 서버에 다음에 붙일 가장 재미있는 기능" placeholder="지속 대화 주제를 입력하세요">
    <button class="btn primary" onclick="act('live_start')">🔁 AI 빠른 대화</button>
    <button class="btn warn" onclick="act('swarm_50')">👥 50명 빠른 라이브 대화</button>
    <button class="btn danger" id="swarmStartBtn" onclick="act('swarm_start')">🚀 800명 스트레스 폭주 채팅</button>
    <button class="btn warn" id="stressBtn" onclick="act('stress_toggle')">🔥 폭주 채팅 ON/OFF</button>
    <button class="btn" onclick="act('stop')">⏸ 멈춤</button>
    <button class="btn" id="swarmStopBtn" onclick="act('swarm_stop')">⏹ 군단 철수</button>
    <button class="btn" onclick="act('warn')">🛡️ 경고·차단 정책 시연</button>
    <div class="status-line" id="demoStatus">준비 중...</div>
  </div>

  <div class="kpi-grid">
    <div class="kpi"><div class="k">방 접속자 (UGameRoom)</div><div class="v" id="kUsers">0</div><div class="s">정원 1,000명 · 20명 페이징</div></div>
    <div class="kpi"><div class="k">TCP RTT (실측)</div><div class="v" id="kRtt">- ms</div><div class="s" id="kRttSub">p50 - / p95 -</div></div>
    <div class="kpi"><div class="k">실시간 패킷 / 채팅</div><div class="v" id="kPkt">0 /s</div><div class="s" id="kChatTot">누적 채팅 0건</div></div>
    <div class="kpi"><div class="k">ServerApp 메모리</div><div class="v" id="kMem">0 MB</div><div class="s" id="kCpu">CPU 0% · 스레드 0</div></div>
    <div class="kpi"><div class="k">.cvtrace 실시간 이벤트</div><div class="v" id="kTrace">0</div><div class="s" id="kTraceKb">0 KB (256B 슬롯)</div></div>
    <div class="kpi"><div class="k">.cvlog 운영 경고</div><div class="v" id="kWarn">0</div><div class="s" id="kLogTot">전체 로그 0건</div></div>
  </div>

  <div class="main-grid">
    <div class="panel">
      <h2><span>실시간 추이 그래프 (최근 30초)</span><span style="font-size:11px; color:#64748b;" id="uptime"></span></h2>
      <div class="charts">
        <div class="chart-box">
          <div class="chart-title"><span>TCP RTT 지연시간 (ms)</span><strong id="cRttVal" style="color:#38bdf8;">0 ms</strong></div>
          <svg id="svgRtt" viewBox="0 0 300 64"></svg>
        </div>
        <div class="chart-box">
          <div class="chart-title"><span>초당 수신 패킷 (pkt/s)</span><strong id="cPktVal" style="color:#4ade80;">0 /s</strong></div>
          <svg id="svgPkt" viewBox="0 0 300 64"></svg>
        </div>
      </div>
      <h2 style="margin-top:2px;"><span>이종 멀티모델 AI Agent 4인 (실시간 모델 전환 가능)</span><span style="font-size:11px; color:#64748b;" id="botTot"></span></h2>
      <div class="agents" id="agentsBox"></div>
    </div>

    <div class="panel">
      <h2><span>실시간 채팅방 스트림 (SC_CHAT · 모델/지연 표기)</span><span style="font-size:11px; color:#64748b;">사람 + 이종 AI</span></h2>
      <div class="feed" id="chatFeed"></div>
      <h2 style="margin-top:2px;"><span>서버 운영 로그 (.cvlog 실시간)</span><span style="font-size:11px; color:#64748b;">traceSeq 연동</span></h2>
      <div class="log-list" id="logList"></div>
    </div>
  </div>

<script>
let agentDomReady = false;
function esc(s) {
  return String(s ?? '').replace(/[&<>"']/g, c => ({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
}
function drawSpark(svgId, vals, color, minMaxFloor) {
  const svg = document.getElementById(svgId);
  if (!vals.length) { svg.innerHTML = ''; return; }
  const maxV = Math.max(minMaxFloor, ...vals);
  const w = 300, h = 60, pad = 4;
  const pts = vals.map((v, i) => {
    const x = vals.length === 1 ? w : (i / (vals.length - 1)) * w;
    const y = h - pad - ((v / maxV) * (h - pad * 2));
    return `${x.toFixed(1)},${y.toFixed(1)}`;
  });
  const area = `0,${h} ${pts.join(' ')} ${w},${h}`;
  svg.innerHTML =
    `<polygon points="${area}" fill="${color}" fill-opacity="0.16"></polygon>` +
    `<polyline points="${pts.join(' ')}" fill="none" stroke="${color}" stroke-width="2.2" stroke-linejoin="round"></polyline>`;
}
async function act(type) {
  const topic = document.getElementById('topicInput').value;
  await fetch('/api/action', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({action: type, topic})
  });
  poll();
}
async function setModel(nick, modelKey) {
  await fetch('/api/set-model', {
    method: 'POST',
    headers: {'Content-Type':'application/json'},
    body: JSON.stringify({nickname: nick, model_key: modelKey})
  });
  poll();
}
function renderAgents(agents, models) {
  const box = document.getElementById('agentsBox');
  if (!agentDomReady) {
    box.innerHTML = agents.map((a, idx) => {
      const opts = models.map(m => `<option value="${esc(m.key)}">${esc(m.label)}</option>`).join('');
      return `<div class="agent" id="ag_${idx}" style="border-left-color:${esc(a.color)}">
        <div class="agent-top">
          <span class="agent-name" style="color:${esc(a.color)}">${esc(a.nickname)}</span>
          <span class="agent-state" id="ag_st_${idx}">대기 중</span>
        </div>
        <div class="agent-meta" id="ag_meta_${idx}">${esc(a.role)}</div>
        <select class="model-sel" id="ag_sel_${idx}" onchange="setModel('${esc(a.nickname)}', this.value)">${opts}</select>
        <div class="agent-last" id="ag_last_${idx}">대기 중...</div>
      </div>`;
    }).join('');
    agentDomReady = true;
  }
  agents.forEach((a, idx) => {
    const st = document.getElementById(`ag_st_${idx}`);
    const meta = document.getElementById(`ag_meta_${idx}`);
    const sel = document.getElementById(`ag_sel_${idx}`);
    const last = document.getElementById(`ag_last_${idx}`);
    if (!st) return;
    st.className = `agent-state ${a.state}`;
    st.textContent = a.state === 'thinking' ? '생각 중...' : (a.state === 'idle' ? '대기 중' : a.state);
    const msTxt = a.last_ms != null ? `${a.last_ms}ms` : '-';
    const sqTxt = a.squad_target > 0 ? ` · 관리 군단 ${a.squad_active}/${a.squad_target}명` : '';
    meta.textContent = `${a.role}${sqTxt} · 발언 ${a.replies}회 · ${a.actual_model} (${msTxt})`;
    if (document.activeElement !== sel) sel.value = a.model_key;
    last.textContent = a.last_text || '대기 중...';
    last.title = a.last_text || '';
  });
}
async function poll() {
  try {
    const d = await (await fetch('/api/metrics')).json();
    document.getElementById('demoStatus').textContent = d.demo_status;
    document.getElementById('uptime').textContent = `${d.server} · 가동 ${d.uptime_s}s`;
    const sq = d.squad || {active:0, target_per_ai:0, deploying:false, rx_mb:0, rx_packets:0};
    document.getElementById('botTot').textContent = sq.active > 0
      ? `군단 ${sq.active}명 가동 · 수신 ${sq.rx_mb} MB · AI 발언 ${d.total_bot_replies}건`
      : `누적 AI 발언 ${d.total_bot_replies}건`;
    const sStart = document.getElementById('swarmStartBtn');
    const sStop = document.getElementById('swarmStopBtn');
    if (sStart) sStart.disabled = sq.deploying;
    if (sStop) sStop.disabled = sq.deploying || sq.active === 0;
    const badge = document.getElementById('srvBadge');
    const btxt = document.getElementById('srvBadgeText');
    if (d.proc.running) {
      badge.className = 'badge';
      btxt.textContent = `ServerApp LIVE · PID ${d.proc.pid}`;
    } else {
      badge.className = 'badge off';
      btxt.textContent = 'ServerApp 오프라인';
    }
    document.getElementById('kUsers').textContent = `${d.room_users}명`;
    document.getElementById('kRtt').textContent = `${d.rtt_ms.latest.toFixed(2)} ms`;
    document.getElementById('kRttSub').textContent = `p50 ${d.rtt_ms.p50.toFixed(2)} / p95 ${d.rtt_ms.p95.toFixed(2)} ms`;
    document.getElementById('kPkt').textContent = `${d.pkt_per_sec} /s`;
    document.getElementById('kChatTot').textContent = `누적 채팅 ${d.total_chats.toLocaleString()}건`;
    document.getElementById('kMem').textContent = `${d.proc.rss_mb.toFixed(1)} MB`;
    document.getElementById('kCpu').textContent = `CPU ${d.proc.cpu_pct.toFixed(1)}% · 스레드 ${d.proc.threads}`;
    document.getElementById('kTrace').textContent = d.trace.events.toLocaleString();
    document.getElementById('kTraceKb').textContent = `${d.trace.trace_kb.toLocaleString()} KB (.cvtrace)`;
    document.getElementById('kWarn').textContent = d.trace.warn_count;
    document.getElementById('kLogTot').textContent = `전체 로그 ${d.trace.log_count}건`;
    const lb = document.getElementById('loadBtn');
    if (lb) lb.disabled = d.load_running;
    const sb = document.getElementById('stressBtn');
    if (sb) sb.textContent = d.stress_mode ? `🔥 폭주 ON (${Number(d.stress_sent || 0).toLocaleString()}건)` : '🔥 폭주 채팅 ON/OFF';

    const rtts = d.series.map(x => x.rtt);
    const pkts = d.series.map(x => x.pkt_s);
    document.getElementById('cRttVal').textContent = `${d.rtt_ms.latest.toFixed(2)} ms`;
    document.getElementById('cPktVal').textContent = `${d.pkt_per_sec} /s`;
    drawSpark('svgRtt', rtts, '#38bdf8', 2.0);
    drawSpark('svgPkt', pkts, '#4ade80', 10.0);

    renderAgents(d.agents, d.models);

    const feed = document.getElementById('chatFeed');
    const atBottom = feed.scrollTop + feed.clientHeight >= feed.scrollHeight - 24;
    feed.innerHTML = d.chat_feed.map(m => {
      const mtag = m.is_bot && m.model_tag
        ? `<span class="mtag">${esc(m.model_tag)}${m.ms != null ? ' · ' + m.ms + 'ms' : ''}</span>`
        : '';
      return `<div class="msg"><span class="tm">[${esc(m.time)}]</span><span class="sp" style="color:${esc(m.color)}">${esc(m.speaker)}</span>${mtag}<span>${esc(m.text)}</span></div>`;
    }).join('');
    if (atBottom) feed.scrollTop = feed.scrollHeight;

    const logs = document.getElementById('logList');
    logs.innerHTML = (d.trace.recent_logs || []).map(l =>
      `<div class="log-row"><span style="color:#64748b">[${esc(l.time)}]</span> <span class="lvl-${esc(l.level)}">${esc(l.level)}</span> <span style="color:#94a3b8">#${esc(l.traceSeq)}</span> ${esc(l.msg)}</div>`
    ).join('') || '<div style="color:#64748b;">로그 대기 중...</div>';
  } catch (e) {}
}
poll();
setInterval(poll, 500);
</script>
</body>
</html>
"""


def start_hud_server(
    hud_port: int,
    telemetry: TelemetryStore,
    orchestrator: DialogueOrchestrator,
    loop: asyncio.AbstractEventLoop,
) -> ThreadingHTTPServer:
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args: Any) -> None:
            return

        def _send(self, code: int, content_type: str, body: bytes) -> None:
            self.send_response(code)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self) -> None:
            if self.path == "/" or self.path.startswith("/?"):
                self._send(200, "text/html; charset=utf-8", HUD_HTML.encode("utf-8"))
            elif self.path.startswith("/api/metrics"):
                data = json.dumps(telemetry.snapshot(), ensure_ascii=False).encode("utf-8")
                self._send(200, "application/json; charset=utf-8", data)
            else:
                self._send(404, "text/plain; charset=utf-8", b"Not Found")

        def do_POST(self) -> None:
            length = int(self.headers.get("Content-Length", "0"))
            raw = self.rfile.read(min(length, 8192))
            try:
                body = json.loads(raw.decode("utf-8"))
            except Exception:
                body = {}

            if self.path == "/api/set-model":
                ok = telemetry.set_agent_model(str(body.get("nickname", "")), str(body.get("model_key", "")))
                self._send(200 if ok else 400, "application/json; charset=utf-8", json.dumps({"ok": ok}).encode())
                return

            if self.path != "/api/action":
                self._send(404, "text/plain; charset=utf-8", b"Not Found")
                return

            action = str(body.get("action", ""))
            topic = str(body.get("topic", "")).strip() or "C++ 채팅 서버에 다음에 붙일 가장 재미있는 기능"

            if action == "live_start":
                loop.call_soon_threadsafe(orchestrator.trigger_live_start, topic)
            elif action == "debate":
                loop.call_soon_threadsafe(orchestrator.trigger_debate, topic, 6, "HUD")
            elif action == "continue":
                loop.call_soon_threadsafe(orchestrator.trigger_continue, 4)
            elif action == "stop":
                loop.call_soon_threadsafe(orchestrator.trigger_stop)
            elif action == "swarm_50":
                loop.call_soon_threadsafe(orchestrator.trigger_swarm_start, 13, 50, True, False, topic)
            elif action in ("swarm_start", "swarm_stress"):
                per_ai = int(body.get("per_ai", 200))
                loop.call_soon_threadsafe(orchestrator.trigger_swarm_start, max(10, min(240, per_ai)), None, True, True, topic)
            elif action == "stress_toggle":
                loop.call_soon_threadsafe(orchestrator.trigger_stress_toggle, topic)
            elif action == "swarm_stop":
                loop.call_soon_threadsafe(orchestrator.trigger_swarm_stop)
            elif action == "load":
                loop.call_soon_threadsafe(orchestrator.trigger_load_burst, 50, 40)
            elif action == "warn":
                loop.call_soon_threadsafe(orchestrator.trigger_warning_probe)

            self._send(200, "application/json; charset=utf-8", b'{"ok":true}')

    server = ThreadingHTTPServer(("127.0.0.1", hud_port), Handler)
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    return server


async def run_agents(args: argparse.Namespace) -> int:
    personas = DEFAULT_PERSONAS[: args.count]
    stop_event = asyncio.Event()
    loop = asyncio.get_running_loop()

    catalog = build_model_catalog(args.gateway, args.ollama)
    router = MultiModelRouter(catalog=catalog, timeout=args.timeout)
    override_key = args.model if (args.model and args.model in catalog) else None

    telemetry = TelemetryStore(
        host=args.host,
        port=args.port,
        trace_dir=Path(args.trace_dir),
        personas=personas,
        catalog=catalog,
        override_model_key=override_key,
    )

    print("=== MultiServer 이종 멀티모델 AI Agent + 실시간 계측 HUD 시작 ===", flush=True)
    print(f"Server  : {args.host}:{args.port}", flush=True)
    print(f"Models  : Ollama({args.ollama}) + Antseed({args.gateway})", flush=True)
    if args.hud_port > 0:
        print(f"LiveHUD : http://127.0.0.1:{args.hud_port}/", flush=True)
    for p in personas:
        mk = telemetry.get_agent_model_key(p.nickname)
        print(f"  - {p.nickname:8s} -> {catalog[mk].label}", flush=True)
    print("명령어  : '/토론 <주제>' | '/계속' | '/멈춰' | '/군단 200' | '/철수' | '/부하 50' | '/경고테스트'", flush=True)

    for sig in (signal.SIGINT, signal.SIGTERM):
        try:
            loop.add_signal_handler(sig, stop_event.set)
        except NotImplementedError:
            pass

    sessions = [ChatAgentSession(p, args.host, args.port, telemetry) for p in personas]
    swarm = SquadSwarmManager(args.host, args.port, personas, telemetry)
    orchestrator = DialogueOrchestrator(
        sessions=sessions,
        router=router,
        telemetry=telemetry,
        swarm=swarm,
        host=args.host,
        port=args.port,
        banter_turns=args.banter_turns,
        turn_pause=args.turn_pause,
        stop_event=stop_event,
    )

    hud_server: Optional[ThreadingHTTPServer] = None
    if args.hud_port > 0:
        hud_server = start_hud_server(args.hud_port, telemetry, orchestrator, loop)

    while not stop_event.is_set():
        try:
            for sess in sessions:
                if not sess.logged_in:
                    await sess.connect_and_login()
                    await asyncio.sleep(0.06)
            break
        except Exception as exc:
            telemetry.demo_status = f"⏳ ServerApp({args.host}:{args.port}) 연결 대기 중... ({exc})"
            for sess in sessions:
                await sess.close()
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=1.5)
            except asyncio.TimeoutError:
                pass

    if not stop_event.is_set():
        telemetry.demo_status = "✅ 이종 멀티모델 AI 4인 입장 완료 — ChatClient에서 말하거나 위 시연 버튼을 눌러 보세요"

    async def pump_session(sess: ChatAgentSession, is_primary: bool) -> None:
        try:
            while not stop_event.is_set():
                read_task = asyncio.create_task(sess.read_packet())
                stop_task = asyncio.create_task(stop_event.wait())
                done, pending = await asyncio.wait(
                    {read_task, stop_task}, return_when=asyncio.FIRST_COMPLETED
                )
                for p in pending:
                    p.cancel()
                if stop_task in done:
                    break
                pkt = read_task.result()
                if pkt is None:
                    print(f"[LEAVE] {sess.persona.nickname} 서버 연결 종료 감지", flush=True)
                    break
                ptype, body = pkt
                if is_primary:
                    if ptype == SC_CHAT:
                        speaker, text = parse_sc_chat(body)
                        orchestrator.on_room_chat(speaker, text)
                    elif ptype == SC_ROOM_USER_COUNT:
                        telemetry.record_packet(is_chat=False)
                        telemetry.set_room_users(parse_sc_room_user_count(body))
                    elif ptype == SC_ROOM_USERS:
                        telemetry.record_packet(is_chat=False)
                        _, tot, _ = parse_sc_room_users(body)
                        sess.on_room_users_response(tot)
        finally:
            await sess.close()

    async def telemetry_loop() -> None:
        primary = sessions[0]
        while not stop_event.is_set():
            try:
                await primary.send_rtt_probe()
                await asyncio.to_thread(telemetry.tick_sample)
            except Exception:
                pass
            try:
                await asyncio.wait_for(stop_event.wait(), timeout=0.5)
            except asyncio.TimeoutError:
                pass

    tasks = [
        asyncio.create_task(pump_session(sess, is_primary=(idx == 0)))
        for idx, sess in enumerate(sessions)
    ]
    tasks.append(asyncio.create_task(telemetry_loop()))

    if args.auto_swarm > 0:
        await asyncio.sleep(0.3)
        orchestrator.trigger_swarm_start(per_ai=max(10, min(240, args.auto_swarm)), auto_live=True)
    elif args.auto_live:
        await asyncio.sleep(0.4)
        orchestrator.trigger_live_start()
    elif args.auto_debate:
        await asyncio.sleep(0.5)
        orchestrator.trigger_debate(args.auto_debate, turns=6, initiator="자동시연")

    try:
        await asyncio.gather(*tasks)
    except KeyboardInterrupt:
        stop_event.set()
        await asyncio.gather(*tasks, return_exceptions=True)
    finally:
        await swarm.withdraw()
        if hud_server:
            hud_server.shutdown()
            hud_server.server_close()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description="MultiServer 이종 멀티모델 AI Agent 그룹 대화 + 실시간 서버 계측 HUD")
    parser.add_argument("--host", default="127.0.0.1", help="MultiServer 호스트")
    parser.add_argument("--port", type=int, default=7777, help="MultiServer 포트")
    parser.add_argument("--hud-port", type=int, default=8766, help="실시간 계측 HUD 웹 포트 (0이면 비활성)")
    parser.add_argument("--gateway", default="http://127.0.0.1:8377", help="Antseed Gateway URL")
    parser.add_argument("--ollama", default="http://127.0.0.1:11434", help="Ollama 로컬 서버 URL")
    parser.add_argument("--model", default="", help="전 에이전트 단일 모델 키 강제 지정 (비우면 이종 멀티모델 기본 배정)")
    parser.add_argument("--count", type=int, default=4, choices=[1, 2, 3, 4], help="AI 에이전트 수")
    parser.add_argument("--banter-turns", type=int, default=3, help="사용자 발언 후 자율 티키타카 턴 수")
    parser.add_argument("--turn-pause", type=float, default=1.4, help="에이전트 발언 간격(초)")
    parser.add_argument("--trace-dir", default=str(DEFAULT_TRACE_DIR), help="ServerApp ChangeViewerTrace 경로")
    parser.add_argument("--auto-swarm", type=int, default=0, help="시작 직후 AI당 군단 자동 투입 수 (예: 200 -> 800명)")
    parser.add_argument("--auto-live", action="store_true", help="시작 직후 이종 멀티모델 지속 라이브 대화 자동 가동")
    parser.add_argument("--auto-debate", default="", help="시작 직후 자동 토론 주제")
    parser.add_argument("--timeout", type=float, default=35.0, help="LLM 응답 타임아웃 초")
    args = parser.parse_args()

    try:
        return asyncio.run(run_agents(args))
    except KeyboardInterrupt:
        print("\n[EXIT] 이종 멀티모델 AI Agent 군단 및 계측 HUD를 종료했습니다.", flush=True)
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
