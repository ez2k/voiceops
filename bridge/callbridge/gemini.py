"""Gemini Live 음성 에이전트 — 통화 음성의 endpoint.

    통화 8kHz ─soxr→ 16kHz ─→ Gemini Live ─→ 24kHz ─soxr→ 통화 8kHz

Gemini 연결은 전용 스레드의 asyncio 루프에서 돈다. play()/take() 는 링크 수신 스레드에서 불린다.
API 키는 환경변수 GEMINI_API_KEY 에서 읽는다.
"""

import asyncio
import json
import threading
from datetime import datetime
from pathlib import Path
from typing import Callable

import numpy as np
import soxr
from google import genai
from google.genai import types

from .audio import CallControl, CallInfo, PcmFifo

MODEL = "gemini-3.8-live"
GEMINI_IN_RATE = 16000    # Gemini 입력: 16bit PCM 16kHz
GEMINI_OUT_RATE = 24000   # Gemini 출력: 16bit PCM 24kHz
SEND_CHUNK_MS = 40        # 통화 음성을 이 길이로 묶어서 보낸다
HANDOFF_TIMEOUT_S = 15    # 끊기·인계 전에 AI 음성이 다 나가기를 기다리는 최대 시간
GREETING_CUE = "(전화가 연결되었습니다. 지금 첫 인사를 하세요.)"

TOOLS = [
    types.Tool(
        function_declarations=[
            types.FunctionDeclaration(
                name="end_call",
                description="통화를 끝낸다. 용건이 끝나 작별 인사를 마친 뒤에만 호출한다.",
                parameters_json_schema={"type": "object", "properties": {}},
            ),
            types.FunctionDeclaration(
                name="transfer_to_human",
                description=(
                    "사람 직원에게 통화를 넘긴다. 상대가 사람을 원하거나 지시문으로 처리할 수 없는 일일 때 호출한다. "
                    "호출하기 전에 담당자에게 연결해 드리겠다고 말한다."
                ),
                parameters_json_schema={
                    "type": "object",
                    "properties": {"reason": {"type": "string", "description": "넘기는 이유"}},
                    "required": ["reason"],
                },
            ),
            types.FunctionDeclaration(
                name="save_note",
                description="사람이 이어받아야 할 용건(예약, 문의, 회신 요청 등)을 접수 메모로 저장한다.",
                parameters_json_schema={
                    "type": "object",
                    "properties": {
                        "name": {"type": "string", "description": "상대 이름 (말했다면)"},
                        "callback_number": {"type": "string", "description": "회신 번호 (발신번호와 다를 때)"},
                        "request": {"type": "string", "description": "용건 요약"},
                    },
                    "required": ["request"],
                },
            ),
        ]
    )
]


class GeminiAgent:
    """
    call         이번 통화 정보 (상대 번호, 기록 파일 경로)
    control      통화 끊기·사람 인계
    instructions 지시문 (prompt.md 내용: 역할, 인사말, 업무 지식, 처리 규칙)
    notes_path   save_note 로 접수 메모를 쌓을 JSONL 파일
    client       genai.Client (테스트용으로 바꿔 끼울 수 있다. 없으면 환경변수 키로 만든다)
    """

    def __init__(
        self,
        call: CallInfo,
        control: CallControl,
        instructions: str,
        notes_path: Path,
        out: Callable[[str], None] = print,
        client: genai.Client | None = None,
    ) -> None:
        self._call = call
        self._control = control
        self._instructions = instructions
        self._notes_path = notes_path
        self._out = out
        self._client = client
        self._said: list[list[str]] = []
        self._background: set[asyncio.Task] = set()

    # ---- AudioEndpoint (링크 수신 스레드) ----

    def start(self, rate: int) -> None:
        self._to_gemini = soxr.ResampleStream(rate, GEMINI_IN_RATE, 1, dtype="int16")
        self._from_gemini = soxr.ResampleStream(GEMINI_OUT_RATE, rate, 1, dtype="int16")
        self._chunk_bytes = GEMINI_IN_RATE * 2 * SEND_CHUNK_MS // 1000
        self._pending = bytearray()
        # AI 음성은 실시간보다 빨리 도착하므로 넉넉히 담고, 60ms 쌓인 뒤부터 내보낸다.
        self._speech = PcmFifo(max_bytes=rate * 2 * 120, prebuffer=rate * 2 * 60 // 1000)
        self._ready = threading.Event()
        self._loop = asyncio.new_event_loop()
        self._queue: asyncio.Queue[bytes] = asyncio.Queue()
        self._stopping = asyncio.Event()
        self._thread = threading.Thread(target=self._loop.run_until_complete, args=(self._main(),),
                                        daemon=True, name="gemini")
        self._thread.start()

    def play(self, pcm: bytes) -> None:
        if not self._ready.is_set():
            return  # 연결 중에 들어온 소리는 버린다 (AI 가 먼저 인사한다)
        self._pending += self._to_gemini.resample_chunk(np.frombuffer(pcm, dtype="<i2")).tobytes()
        if len(self._pending) >= self._chunk_bytes:
            chunk = bytes(self._pending)
            self._pending.clear()
            try:
                self._loop.call_soon_threadsafe(self._queue.put_nowait, chunk)
            except RuntimeError:
                pass  # 루프가 이미 끝났다

    def take(self, nbytes: int) -> bytes:
        return self._speech.read(nbytes)

    def stop(self) -> None:
        try:
            self._loop.call_soon_threadsafe(self._stopping.set)
        except RuntimeError:
            pass
        self._thread.join(timeout=5)
        if not self._thread.is_alive():
            self._loop.close()

    # ---- Gemini 세션 (gemini 스레드) ----

    async def _main(self) -> None:
        config = types.LiveConnectConfig(
            response_modalities=[types.Modality.AUDIO],
            system_instruction=self._system_instruction(),
            tools=TOOLS,
            input_audio_transcription=types.AudioTranscriptionConfig(),
            output_audio_transcription=types.AudioTranscriptionConfig(),
        )
        try:
            client = self._client or genai.Client()
            async with client.aio.live.connect(model=MODEL, config=config) as session:
                self._ready.set()
                await session.send_client_content(
                    turns=types.Content(role="user", parts=[types.Part(text=GREETING_CUE)]), turn_complete=True
                )
                workers = [asyncio.create_task(self._send_audio(session)), asyncio.create_task(self._receive(session))]
                stop = asyncio.create_task(self._stopping.wait())
                done, pending = await asyncio.wait([*workers, stop], return_when=asyncio.FIRST_COMPLETED)
                for task in pending:
                    task.cancel()
                await asyncio.gather(*pending, return_exceptions=True)
                if not self._stopping.is_set():
                    for task in done:
                        if task.exception() is not None:
                            raise task.exception()
                    raise ConnectionError("Gemini 세션이 끝났습니다")
        except Exception as e:
            if not self._stopping.is_set():
                self._out(f"[AI 오류] {e!r} — 사람에게 넘깁니다")
                self._control.handover()
        finally:
            self._ready.clear()
            for task in self._background:
                task.cancel()
            await asyncio.gather(*self._background, return_exceptions=True)
            self._flush_transcript()

    def _system_instruction(self) -> str:
        return (
            f"{self._instructions.strip()}\n\n"
            "## 이번 통화 정보\n"
            f"- 상대 전화번호: {self._call.number or '알 수 없음'}\n"
            f"- 현재 시각: {datetime.now():%Y-%m-%d %H:%M}\n"
        )

    async def _send_audio(self, session) -> None:
        mime = f"audio/pcm;rate={GEMINI_IN_RATE}"
        while True:
            chunk = await self._queue.get()
            await session.send_realtime_input(audio=types.Blob(data=chunk, mime_type=mime))

    async def _receive(self, session) -> None:
        while True:
            got_any = False
            async for msg in session.receive():  # 모델 한 턴이 끝나면 반복이 끝난다
                got_any = True
                await self._handle(session, msg)
            if not got_any:
                return  # 연결이 닫혔다

    async def _handle(self, session, msg: types.LiveServerMessage) -> None:
        if msg.data:
            pcm = self._from_gemini.resample_chunk(np.frombuffer(msg.data, dtype="<i2"))
            self._speech.write(pcm.tobytes())

        sc = msg.server_content
        if sc is not None:
            if sc.interrupted:
                self._speech.clear()  # 상대가 말을 끊으면 AI 음성을 바로 멈춘다
            if sc.input_transcription and sc.input_transcription.text:
                self._add_transcript("상대", sc.input_transcription.text)
            if sc.output_transcription and sc.output_transcription.text:
                self._add_transcript("AI", sc.output_transcription.text)
            if sc.turn_complete or sc.interrupted:
                self._flush_transcript()

        if msg.tool_call and msg.tool_call.function_calls:
            responses = [self._run_tool(fc) for fc in msg.tool_call.function_calls]
            await session.send_tool_response(function_responses=responses)

        if msg.go_away is not None:
            self._out(f"[AI] 세션 종료 예고: {msg.go_away.time_left} 남음")

    # ---- 도구 ----

    def _run_tool(self, fc: types.FunctionCall) -> types.FunctionResponse:
        args = dict(fc.args or {})
        self._out(f"[AI 도구] {fc.name} {json.dumps(args, ensure_ascii=False)}")
        silent = types.FunctionResponseScheduling.SILENT
        when_idle = types.FunctionResponseScheduling.WHEN_IDLE

        if fc.name == "end_call":
            self._after_speech(self._control.hangup)
            result, scheduling = {"ok": True}, silent
        elif fc.name == "transfer_to_human":
            self._after_speech(self._control.handover)
            result, scheduling = {"ok": True}, silent
        elif fc.name == "save_note":
            self._save_note(args)
            result, scheduling = {"ok": True, "message": "접수 메모를 저장했습니다"}, when_idle
        else:
            result, scheduling = {"error": f"알 수 없는 도구: {fc.name}"}, when_idle
        return types.FunctionResponse(id=fc.id, name=fc.name, response=result, scheduling=scheduling)

    def _after_speech(self, action: Callable[[], None]) -> None:
        """AI 가 하던 말(작별 인사, '연결해 드리겠습니다')이 다 나간 뒤에 action 을 실행한다."""

        async def wait_then_act() -> None:
            loop = asyncio.get_running_loop()
            deadline = loop.time() + HANDOFF_TIMEOUT_S
            await asyncio.sleep(0.5)  # 마지막 음성 조각이 도착할 여유
            while self._speech.pending() > 0 and loop.time() < deadline:
                await asyncio.sleep(0.1)
            await asyncio.sleep(0.3)
            action()

        task = asyncio.get_running_loop().create_task(wait_then_act())
        self._background.add(task)
        task.add_done_callback(self._background.discard)

    def _save_note(self, args: dict) -> None:
        note = {"time": datetime.now().isoformat(timespec="seconds"), "caller": self._call.number, **args}
        self._notes_path.parent.mkdir(parents=True, exist_ok=True)
        with open(self._notes_path, "a", encoding="utf-8") as f:
            f.write(json.dumps(note, ensure_ascii=False) + "\n")

    # ---- 대화록 ----

    def _add_transcript(self, who: str, text: str) -> None:
        if self._said and self._said[-1][0] == who:
            self._said[-1][1] += text
        else:
            self._said.append([who, text])

    def _flush_transcript(self) -> None:
        if not self._said:
            return
        path = self._call.record_base.with_suffix(".txt")
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "a", encoding="utf-8") as f:
            for who, text in self._said:
                line = f"{who}: {text.strip()}"
                f.write(line + "\n")
                self._out(f"[대화] {line}")
        self._said = []
