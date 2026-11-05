"""중계 서버를 실제 WebSocket 으로 띄우고, 펌웨어처럼 말하는 가짜 기기와 가짜 Gemini 로 왕복을 확인한다 (네트워크 없음).

기기 쪽 메시지는 펌웨어(standalone/main/realtime.c, realtime_messages.h)가 보내는 것과 같은 모양이다.
"""

import asyncio
import base64
import contextlib
import json

import numpy as np
import pytest
from google.genai import types
from websockets.asyncio.client import connect
from websockets.exceptions import InvalidStatus

from geminirelay import session, ulaw
from geminirelay.server import make_server

TOKEN = "test-token-0123456789"
TOOLS = [
    {"type": "function", "name": "end_call", "description": "통화를 끝낸다", "parameters": {"type": "object", "properties": {}}},
    {"type": "function", "name": "save_note", "description": "접수 메모",
     "parameters": {"type": "object", "properties": {"request": {"type": "string"}}, "required": ["request"]}},
]


class FakeGemini:
    """대본(turn) 을 테스트가 그때그때 넣는다. receive() 는 한 턴씩 돌려준다 (SDK 와 같은 방식)."""

    def __init__(self):
        self.turns: asyncio.Queue = asyncio.Queue()
        self.client_content, self.audio, self.tool_responses = [], [], []
        self.connect_args = None
        self.closed = False

    async def send_client_content(self, *, turns, turn_complete=True):
        self.client_content.append(turns)

    async def send_realtime_input(self, *, audio):
        self.audio.append(audio)

    async def send_tool_response(self, *, function_responses):
        self.tool_responses.extend(function_responses)

    async def receive(self):
        for msg in await self.turns.get():
            yield msg


class FakeClient:
    def __init__(self, gemini: FakeGemini):
        outer = self
        self.gemini = gemini

        class Live:
            @contextlib.asynccontextmanager
            async def connect(self, *, model, config):
                gemini.connect_args = (model, config)
                try:
                    yield gemini
                finally:
                    gemini.closed = True

        class Aio:
            live = Live()

        self.aio = Aio()


def gemini_audio(seconds: float, said: str | None = None, turn_complete=False):
    pcm = (np.sin(np.arange(int(24000 * seconds)) * 2 * np.pi * 440 / 24000) * 8000).astype("<i2").tobytes()
    return types.LiveServerMessage(server_content=types.LiveServerContent(
        model_turn=types.Content(parts=[types.Part(inline_data=types.Blob(data=pcm, mime_type="audio/pcm;rate=24000"))]),
        output_transcription=types.Transcription(text=said) if said else None,
        turn_complete=turn_complete,
    ))


async def recv_until(ws, kind: str, timeout=3.0) -> tuple[dict, list[dict]]:
    """kind 메시지가 올 때까지 받는다. (그 메시지, 그 전에 받은 것들)"""
    seen = []
    while True:
        msg = json.loads(await asyncio.wait_for(ws.recv(), timeout))
        if msg["type"] == kind:
            return msg, seen
        seen.append(msg)


async def ready(ws) -> None:
    """펌웨어처럼 세션을 연다 (session.created → session.update → session.updated)"""
    assert json.loads(await ws.recv())["type"] == "session.created"
    await ws.send(json.dumps({"type": "session.update", "session": {"instructions": "x"}}))
    await recv_until(ws, "session.updated")


@pytest.fixture
def relay(tmp_path):
    gemini = FakeGemini()

    @contextlib.asynccontextmanager
    async def running():
        async with make_server("127.0.0.1", 0, TOKEN, FakeClient(gemini), "gemini-3.8-live", tmp_path) as server:
            port = server.sockets[0].getsockname()[1]
            yield f"ws://127.0.0.1:{port}/v1/realtime?model=gpt-realtime-2.1-mini", gemini

    return running, tmp_path


def test_rejects_wrong_token(relay):
    running, _ = relay

    async def go():
        async with running() as (url, _):
            with pytest.raises(InvalidStatus) as e:
                async with connect(url, additional_headers={"Authorization": "Bearer nope"}):
                    pass
            assert e.value.response.status_code == 401

    asyncio.run(go())


def test_call_flow(relay):
    running, data_dir = relay

    async def go():
        async with running() as (url, gemini):
            async with connect(url, additional_headers={"Authorization": f"Bearer {TOKEN}"}) as ws:
                assert json.loads(await ws.recv())["type"] == "session.created"

                # 펌웨어: 연결되면 session.update → 준비되면 발신 번호·첫 인사·대답 요청
                await ws.send(json.dumps({"type": "session.update", "session": {
                    "type": "realtime", "output_modalities": ["audio"], "instructions": "# 역할\nOO회사 AI 직원",
                    "audio": {"input": {"format": {"type": "audio/pcmu"}, "turn_detection": {"type": "server_vad"}},
                              "output": {"format": {"type": "audio/pcmu"}, "voice": "marin"}},
                    "tools": TOOLS, "tool_choice": "auto"}}))
                await recv_until(ws, "session.updated")
                model, config = gemini.connect_args
                assert model == "gemini-3.8-live"  # 기기의 gpt-… 모델 이름 대신 Gemini 기본값
                assert config.system_instruction == "# 역할\nOO회사 AI 직원"
                assert [f.name for f in config.tools[0].function_declarations] == ["end_call", "save_note"]
                assert config.input_audio_transcription is not None and config.output_audio_transcription is not None

                for text, role in (("이번 통화 정보: 상대 전화번호 01012345678", "system"),
                                   ("(전화가 연결되었습니다. 지금 첫 인사를 하세요.)", "user")):
                    await ws.send(json.dumps({"type": "conversation.item.create", "item": {
                        "type": "message", "role": role, "content": [{"type": "input_text", "text": text}]}}))
                await ws.send(json.dumps({"type": "response.create"}))

                # 상대 음성 0.1초 (μ-law 800 바이트) → Gemini 에는 8kHz PCM
                tone = (np.sin(np.arange(800) / 5) * 6000).astype(np.int16)
                await ws.send(json.dumps({"type": "input_audio_buffer.append",
                                          "audio": base64.b64encode(ulaw.encode(tone)).decode()}))
                await asyncio.sleep(0.2)
                assert len(gemini.client_content) == 1
                turn_text = gemini.client_content[0].parts[0].text
                assert "01012345678" in turn_text and "첫 인사" in turn_text
                assert gemini.audio[0].mime_type == "audio/pcm;rate=8000" and len(gemini.audio[0].data) == 1600

                # AI 인사 0.5초 → μ-law 8kHz 로 약 4000 바이트, 자막, 대답 끝 (토큰 사용량 포함)
                await gemini.turns.put([
                    gemini_audio(0.25, "안녕하세요, "),
                    gemini_audio(0.25, "OO회사 AI 직원입니다."),
                    types.LiveServerMessage(
                        server_content=types.LiveServerContent(turn_complete=True),
                        usage_metadata=types.UsageMetadata(
                            prompt_token_count=300, response_token_count=120,
                            prompt_tokens_details=[types.ModalityTokenCount(modality=types.MediaModality.TEXT, token_count=280),
                                                   types.ModalityTokenCount(modality=types.MediaModality.AUDIO, token_count=20)],
                            response_tokens_details=[types.ModalityTokenCount(modality=types.MediaModality.AUDIO, token_count=120)])),
                ])
                done, before = await recv_until(ws, "response.done")
                kinds = [m["type"] for m in before]
                assert kinds[0] == "response.created"
                deltas = [m for m in before if m["type"] == "response.output_audio.delta"]
                audio = b"".join(base64.b64decode(m["delta"]) for m in deltas)
                assert 3900 <= len(audio) <= 4000
                assert len({m["item_id"] for m in deltas}) == 1
                assert all(len(base64.b64decode(m["delta"])) == 800 for m in deltas[:-1])  # 0.1초씩 묶음
                assert "response.output_audio.done" in kinds
                transcript = next(m for m in before if m["type"] == "response.output_audio_transcript.done")
                assert transcript["transcript"] == "안녕하세요, OO회사 AI 직원입니다."
                assert done["response"]["status"] == "completed"
                assert done["response"]["usage"]["input_token_details"]["audio_tokens"] == 20
                assert done["response"]["usage"]["output_token_details"]["audio_tokens"] == 120

                # 상대가 말한 내용(자막) → 다음 대답 전에 기기로
                await gemini.turns.put([
                    types.LiveServerMessage(server_content=types.LiveServerContent(
                        input_transcription=types.Transcription(text="견적 받고 싶어요"))),
                    types.LiveServerMessage(tool_call=types.LiveServerToolCall(function_calls=[
                        types.FunctionCall(id="call_1", name="save_note", args={"request": "견적"})])),
                ])
                heard, _ = await recv_until(ws, "conversation.item.input_audio_transcription.completed")
                assert heard["transcript"] == "견적 받고 싶어요"
                done, _ = await recv_until(ws, "response.done")
                call = done["response"]["output"][0]
                assert call == {"type": "function_call", "name": "save_note", "call_id": "call_1",
                                "arguments": json.dumps({"request": "견적"}, ensure_ascii=False)}

                # 펌웨어: function_call_output + response.create → Gemini 에는 tool_response
                await ws.send(json.dumps({"type": "conversation.item.create", "item": {
                    "type": "function_call_output", "call_id": "call_1",
                    "output": json.dumps({"ok": True, "message": "접수 메모를 저장했습니다"}, ensure_ascii=False)}}))
                await ws.send(json.dumps({"type": "response.create"}))
                await asyncio.sleep(0.2)
                fr = gemini.tool_responses[0]
                assert (fr.id, fr.name, fr.response["ok"]) == ("call_1", "save_note", True)

                # 상대가 끼어듦 → speech_started + 취소된 대답
                await gemini.turns.put([gemini_audio(0.1),
                                        types.LiveServerMessage(server_content=types.LiveServerContent(interrupted=True))])
                started, _ = await recv_until(ws, "input_audio_buffer.speech_started")
                assert started["item_id"].startswith("item_")
                done, _ = await recv_until(ws, "response.done")
                assert done["response"]["status"] == "cancelled"

                # 펌웨어가 보내는 truncate 는 조용히 무시
                await ws.send(json.dumps({"type": "conversation.item.truncate", "item_id": "item_9",
                                          "content_index": 0, "audio_end_ms": 120}))
            await asyncio.sleep(0.2)
            assert gemini.closed  # 기기가 끊으면 Gemini 세션도 닫는다

        notes = (data_dir / "notes.jsonl").read_text(encoding="utf-8").splitlines()
        assert json.loads(notes[0])["caller"] == "01012345678"
        assert json.loads(notes[0])["request"] == "견적"
        import wave
        with wave.open(str(next(data_dir.glob("*_01012345678_caller.wav")))) as w:
            assert (w.getframerate(), w.getnframes()) == (8000, 800)      # 기기에서 온 0.1초
        with wave.open(str(next(data_dir.glob("*_01012345678_ai.wav")))) as w:
            assert w.getframerate() == 8000 and w.getnframes() >= 3900   # 기기로 보낸 AI 소리
        transcripts = list(data_dir.glob("*_01012345678.txt"))
        assert transcripts and "상대: 견적 받고 싶어요" in transcripts[0].read_text(encoding="utf-8")

    asyncio.run(go())


def test_gemini_failure_reports_error(relay):
    running, _ = relay

    async def go():
        async with running() as (url, gemini):
            async with connect(url, additional_headers={"Authorization": f"Bearer {TOKEN}"}) as ws:
                await ws.recv()
                await ws.send(json.dumps({"type": "session.update", "session": {"instructions": "x"}}))
                await recv_until(ws, "session.updated")
                await gemini.turns.put([])  # 빈 턴 = Gemini 연결이 닫혔다
                err, _ = await recv_until(ws, "error")
                assert err["error"]["code"] == "upstream_error"

    asyncio.run(go())


def test_paces_ai_audio(relay, monkeypatch):
    """Gemini 가 1초 분량을 한꺼번에 줘도 기기에는 실시간 속도로 (AUDIO_LEAD_S 만큼만 앞서) 간다"""
    monkeypatch.setattr(session, "AUDIO_LEAD_S", 0.2)
    running, _ = relay

    async def go():
        loop = asyncio.get_running_loop()
        async with running() as (url, gemini):
            async with connect(url, additional_headers={"Authorization": f"Bearer {TOKEN}"}) as ws:
                await ready(ws)
                await gemini.turns.put([gemini_audio(1.0, "네"),
                                        types.LiveServerMessage(server_content=types.LiveServerContent(turn_complete=True))])
                arrivals = []
                while True:
                    msg = json.loads(await asyncio.wait_for(ws.recv(), 3))
                    if msg["type"] == "response.output_audio.delta":
                        arrivals.append(loop.time())
                    elif msg["type"] == "response.done":
                        done_at = loop.time()
                        break
                assert len(arrivals) >= 9                      # 0.1초 조각 약 10개
                assert arrivals[2] - arrivals[0] < 0.1         # 앞선 0.2초 + 첫 조각은 바로
                assert 0.6 <= arrivals[-1] - arrivals[0] < 1.5  # 나머지는 0.1초마다 → 마지막은 약 0.7초 뒤
                assert done_at >= arrivals[-1]                 # 대답 끝은 음성을 다 보낸 뒤

    asyncio.run(go())


def test_interrupt_drops_unsent_audio(relay, monkeypatch):
    """상대가 끼어들면 아직 안 보낸 AI 음성은 버리고, 끼어듦 알림은 바로 간다"""
    monkeypatch.setattr(session, "AUDIO_LEAD_S", 0.2)
    running, data_dir = relay

    async def go():
        loop = asyncio.get_running_loop()
        async with running() as (url, gemini):
            async with connect(url, additional_headers={"Authorization": f"Bearer {TOKEN}"}) as ws:
                await ready(ws)
                await gemini.turns.put([gemini_audio(2.0)])  # 2초 = 0.1초 조각 20개
                await recv_until(ws, "response.output_audio.delta")
                await asyncio.sleep(0.3)
                t = loop.time()
                await gemini.turns.put([types.LiveServerMessage(server_content=types.LiveServerContent(interrupted=True))])
                _, before = await recv_until(ws, "input_audio_buffer.speech_started")
                assert loop.time() - t < 0.3
                sent = 1 + sum(m["type"] == "response.output_audio.delta" for m in before)
                assert sent <= 10
                done, _ = await recv_until(ws, "response.done")
                assert done["response"]["status"] == "cancelled"
            await asyncio.sleep(0.2)
        import wave
        with wave.open(str(next(data_dir.glob("*_ai.wav")))) as w:
            assert w.getnframes() == sent * 800  # 녹음도 기기로 실제 보낸 만큼만

    asyncio.run(go())
