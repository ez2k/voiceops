"""WebSocket 서버: 기기를 토큰으로 확인하고, 연결마다 DeviceSession 을 돌린다."""

import asyncio
import hmac
import ipaddress
import json
import logging
from http import HTTPStatus
from pathlib import Path
from urllib.parse import parse_qs, urlsplit

from google import genai
from websockets.asyncio.server import Server, ServerConnection, serve
from websockets.http11 import Request, Response

from . import tcpinfo
from .session import DeviceSession

log = logging.getLogger("geminirelay")

PATH = "/v1/realtime"


def make_server(host: str, port: int, token: str, client: genai.Client, default_model: str, data_dir: Path,
                record: bool = True):
    """serve() 를 돌려준다 (`async with` 로 쓴다)."""

    def check(connection: ServerConnection, request: Request) -> Response | None:
        if urlsplit(request.path).path != PATH:
            return connection.respond(HTTPStatus.NOT_FOUND, "not found\n")
        auth = request.headers.get("Authorization", "")
        if not hmac.compare_digest(auth.encode(), f"Bearer {token}".encode()):
            log.warning("%s 토큰이 맞지 않아 거절", connection.remote_address)
            return connection.respond(HTTPStatus.UNAUTHORIZED, "invalid token\n")
        return None

    async def handle(ws: ServerConnection) -> None:
        query = parse_qs(urlsplit(ws.request.path).query)
        model = (query.get("model") or [""])[0]
        if not model.startswith("gemini"):
            model = default_model  # 기기의 기본값(gpt-…)이 와도 Gemini 로

        async def messages():
            async for raw in ws:
                try:
                    msg = json.loads(raw)
                except (json.JSONDecodeError, TypeError):
                    continue
                if isinstance(msg, dict):
                    yield msg

        async def send(msg: dict) -> None:
            await ws.send(json.dumps(msg, ensure_ascii=False))

        peer = f"{ws.remote_address[0]}" if ws.remote_address else "?"
        log.info("%s 기기 연결", peer)
        # 기기와 바로 이어졌을 때만 망 상태를 남긴다 (Caddy 뒤라면 127.0.0.1 ↔ Caddy 구간이라 의미가 없다)
        watcher = None
        if not _is_loopback(peer):
            watcher = asyncio.create_task(tcpinfo.watch(ws.transport.get_extra_info("socket"), peer,
                                                         ws.transport.get_write_buffer_size))
        try:
            await DeviceSession(send, messages(), client, model, data_dir, peer, record).run()
        finally:
            if watcher is not None:
                watcher.cancel()

    # 기기(ESP32)는 메모리가 적다: 압축(permessage-deflate)을 쓰지 않는다
    return serve(handle, host, port, process_request=check, compression=None, max_size=2**20)


def _is_loopback(host: str) -> bool:
    try:
        return ipaddress.ip_address(host).is_loopback
    except ValueError:
        return False


__all__ = ["make_server", "Server", "PATH"]
