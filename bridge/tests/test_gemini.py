"""가짜 Gemini Live 세션으로 GeminiAgent 의 음성 왕복·도구·대화록을 확인한다 (네트워크 없음)."""

import asyncio
import contextlib
import json
import time

import numpy as np
from google.genai import types

from callbridge.audio import CallInfo
from callbridge.gemini import GEMINI_OUT_RATE, GeminiAgent


def audio_msg(seconds, text=None, turn_complete=False):
    pcm = (np.sin(np.arange(int(GEMINI_OUT_RATE * seconds)) / 10) * 8000).astype("<i2").tobytes()
    return types.LiveServerMessage(
        server_content=types.LiveServerContent(
            model_turn=types.Content(parts=[types.Part(inline_data=types.Blob(data=pcm, mime_type="audio/pcm;rate=24000"))]),
            output_transcription=types.Transcription(text=text) if text else None,
            turn_complete=turn_complete,
        )
    )


def tool_msg(call_id, name, args):
    return types.LiveServerMessage(
        tool_call=types.LiveServerToolCall(function_calls=[types.FunctionCall(id=call_id, name=name, args=args)])
    )


def caller_said(text):
    return types.LiveServerMessage(server_content=types.LiveServerContent(input_transcription=types.Transcription(text=text)))


class FakeSession:
    def __init__(self, turns):
        self.turns = list(turns)
        self.client_content = []
        self.audio = []
        self.tool_responses = []
        self.closed = asyncio.Event()

    async def send_client_content(self, *, turns, turn_complete=True):
        self.client_content.append(turns)

    async def send_realtime_input(self, *, audio):
        self.audio.append(audio)

    async def send_tool_response(self, *, function_responses):
        self.tool_responses.extend(function_responses)

    async def receive(self):
        if not self.turns:
            await self.closed.wait()  # 대본이 끝나면 연결이 열린 채로 기다린다
            return
        for msg in self.turns.pop(0):
            yield msg


class FakeClient:
    def __init__(self, session):
        self.session = session
        self.connect_args = None
        outer = self

        class Live:
            @contextlib.asynccontextmanager
            async def connect(self, *, model, config):
                outer.connect_args = (model, config)
                yield outer.session

        class Aio:
            live = Live()

        self.aio = Aio()


class FakeControl:
    def __init__(self):
        self.calls = []

    def hangup(self):
        self.calls.append("hangup")

    def handover(self):
        self.calls.append("handover")


def wait_for(cond, timeout=5.0):
    deadline = time.time() + timeout
    while time.time() < deadline:
        if cond():
            return True
        time.sleep(0.02)
    return False


def test_agent_roundtrip_tools_and_transcript(tmp_path):
    session = FakeSession([
        [audio_msg(0.5, "안녕하세요, OO회사 AI 직원입니다.", turn_complete=True)],
        [caller_said("견적 문의드리려고요"), tool_msg("n1", "save_note", {"name": "홍길동", "request": "견적 문의"})],
        [audio_msg(0.3, "감사합니다. 좋은 하루 되세요.", turn_complete=True), tool_msg("e1", "end_call", {})],
    ])
    client = FakeClient(session)
    control = FakeControl()
    out = []
    call = CallInfo("01012345678", tmp_path / "20261104-120000_01012345678")
    agent = GeminiAgent(call, control, "테스트 지시문", tmp_path / "notes.jsonl", out=out.append, client=client)

    agent.start(8000)
    try:
        # 연결되면 첫 인사 신호를 보낸다
        assert wait_for(lambda: session.client_content)
        model, config = client.connect_args
        assert model == "gemini-3.8-live"
        assert "01012345678" in config.system_instruction and "테스트 지시문" in config.system_instruction

        # 통화 음성(8kHz)은 16kHz 로 바뀌어 40ms(1280바이트) 이상씩 묶여 간다
        for _ in range(50):
            agent.play(np.zeros(60, dtype="<i2").tobytes())  # 7.5ms 프레임 × 50 = 375ms
        assert wait_for(lambda: sum(len(b.data) for b in session.audio) >= 8000)  # 리샘플러 지연(~64ms)을 뺀 양
        assert all(b.mime_type == "audio/pcm;rate=16000" and len(b.data) >= 1280 for b in session.audio)

        # 도구: 메모 저장 + 응답
        assert wait_for(lambda: len(session.tool_responses) == 2)
        assert [(r.id, r.name) for r in session.tool_responses] == [("n1", "save_note"), ("e1", "end_call")]
        note = json.loads((tmp_path / "notes.jsonl").read_text(encoding="utf-8"))
        assert note["caller"] == "01012345678" and note["request"] == "견적 문의" and note["name"] == "홍길동"

        # AI 음성(24kHz 0.8초)이 통화 8kHz 로 나온다. 다 나가기 전에는 끊지 않는다
        got = b""
        while len(got) < 8000 * 2 * 0.7:
            got += agent.take(120)
        assert np.abs(np.frombuffer(got, dtype="<i2")).max() > 1000
        assert control.calls == []
        for _ in range(200):
            agent.take(120)  # 남은 음성을 비운다
        assert wait_for(lambda: control.calls == ["hangup"])
    finally:
        agent.stop()

    transcript = call.record_base.with_suffix(".txt").read_text(encoding="utf-8").splitlines()
    assert transcript == [
        "AI: 안녕하세요, OO회사 AI 직원입니다.",
        "상대: 견적 문의드리려고요",
        "AI: 감사합니다. 좋은 하루 되세요.",
    ]


def test_interruption_clears_ai_speech(tmp_path):
    interrupted = types.LiveServerMessage(server_content=types.LiveServerContent(interrupted=True))
    session = FakeSession([[audio_msg(1.0)], [interrupted]])
    agent = GeminiAgent(CallInfo(None, tmp_path / "x"), FakeControl(), "지시문", tmp_path / "n.jsonl",
                        out=lambda s: None, client=FakeClient(session))
    agent.start(8000)
    try:
        assert wait_for(lambda: not session.turns)
        time.sleep(0.1)
        assert agent.take(1600) == bytes(1600)  # 끊긴 AI 음성은 버려졌다
    finally:
        agent.stop()


def test_connection_failure_hands_over_to_human(tmp_path):
    class BrokenClient:
        class aio:
            class live:
                @staticmethod
                @contextlib.asynccontextmanager
                async def connect(*, model, config):
                    raise ConnectionError("API key invalid")
                    yield

    control = FakeControl()
    out = []
    agent = GeminiAgent(CallInfo(None, tmp_path / "x"), control, "지시문", tmp_path / "n.jsonl",
                        out=out.append, client=BrokenClient())
    agent.start(8000)
    assert wait_for(lambda: control.calls == ["handover"])
    agent.stop()
    assert any("AI 오류" in line for line in out)
