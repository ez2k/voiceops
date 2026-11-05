"""서버 → 기기 TCP 연결 상태 (리눅스 TCP_INFO — `ss -ti` 가 보여 주는 것과 같은 값).

통화 중 AI 소리가 끊기면 망에서 막혔는지 이것으로 가린다: 기기가 실제로 받은 양, 재전송, 응답 지연.
"""

import asyncio
import logging
import socket
import struct
from collections.abc import Callable
from dataclasses import dataclass

log = logging.getLogger("geminirelay")

# struct tcp_info (linux/tcp.h) 앞부분: 8 바이트 + 32비트 24개 + 64비트 4개 + 32비트 4개 (리눅스 4.6 이상)
_TCP_INFO = struct.Struct("=8B24I4Q4I")


@dataclass
class TcpInfo:
    backoff: int        # 재전송 대기 시간이 몇 번 두 배로 늘었나 (0 이 정상)
    rto_ms: int         # 응답이 없을 때 다시 보내기까지 기다리는 시간
    snd_mss: int
    unacked: int        # 보냈는데 아직 기기 응답(ACK)이 없는 조각 수
    last_ack_ms: int    # 기기의 마지막 응답이 몇 ms 전인가
    rtt_ms: int
    total_retrans: int  # 다시 보낸 조각 (누적)
    bytes_acked: int    # 기기가 받았다고 응답한 바이트 (누적)
    notsent: int        # 아직 보내지도 못하고 쌓여 있는 바이트


def read(sock: socket.socket) -> TcpInfo | None:
    try:
        raw = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, _TCP_INFO.size)
    except (OSError, AttributeError):  # 리눅스가 아니거나 이미 닫힌 소켓
        return None
    if len(raw) < _TCP_INFO.size:
        return None
    f = _TCP_INFO.unpack_from(raw)
    return TcpInfo(backoff=f[4], rto_ms=f[8] // 1000, snd_mss=f[10], unacked=f[12], last_ack_ms=f[20],
                   rtt_ms=f[23] // 1000, total_retrans=f[31], bytes_acked=f[34], notsent=f[38])


def describe(prev: TcpInfo, cur: TcpInfo, seconds: float, buffered: int = 0) -> str:
    """5초 동안의 변화를 한 줄로. buffered = 커널에 넘기기 전 프로그램 안에 쌓인 바이트"""
    received = (cur.bytes_acked - prev.bytes_acked) / seconds / 1024
    backlog = (buffered + cur.notsent + cur.unacked * cur.snd_mss) / 1024
    rto = f"{cur.rto_ms}ms" + (f"(×{2 ** cur.backoff})" if cur.backoff else "")
    return (f"기기 쪽 TCP: 받음 {received:.1f}KB/s · 재전송 {cur.total_retrans - prev.total_retrans}회 · "
            f"rtt {cur.rtt_ms}ms · rto {rto} · 밀림 {backlog:.1f}KB · 마지막 응답 {cur.last_ack_ms}ms 전")


async def watch(sock: socket.socket, label: str, buffered: Callable[[], int] = lambda: 0, every: float = 5.0) -> None:
    """연결이 살아 있는 동안 every 초마다 로그를 남긴다 (취소해서 멈춘다)."""
    prev = read(sock)
    if prev is None:
        return
    while True:
        await asyncio.sleep(every)
        cur = read(sock)
        if cur is None:
            return
        log.info("%s %s", label, describe(prev, cur, every, buffered()))
        prev = cur
