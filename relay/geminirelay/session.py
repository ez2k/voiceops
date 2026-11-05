"""기기 연결 하나 ⇄ Gemini Live 세션 하나.

기기(펌웨어 realtime.c)가 보내는 것         →  Gemini 로
  session.update (instructions, tools)       →  세션 설정 (system_instruction, tools, 자막 켜기)
  conversation.item.create (글 메시지)        →  모아 두었다가
  response.create                             →  client_content 한 턴 (또는 도구 결과)
  conversation.item.create (function_call_output) → tool_response
  input_audio_buffer.append (μ-law 8kHz)      →  realtime_input (PCM 8kHz, Gemini 가 변환)
  conversation.item.truncate                  →  (Gemini 는 끼어들기를 스스로 처리하므로 무시)

Gemini 가 보내는 것                         →  기기로
  음성 24kHz                                  →  response.created + response.output_audio.delta (μ-law 8kHz)
  자막 (상대·AI)                              →  …input_audio_transcription.completed / response.output_audio_transcript.done
  interrupted (상대가 끼어듦)                 →  input_audio_buffer.speech_started + response.done (cancelled)
  turn_complete                               →  response.output_audio.done + response.done (토큰 사용량)
  tool_call                                   →  response.done 의 function_call 항목
"""

import asyncio
import base64
import json
import logging
from datetime import datetime
from pathlib import Path

import numpy as np
import soxr
from google import genai
from google.genai import types

from . import ulaw

log = logging.getLogger("geminirelay")

CALL_RATE = 8000        # 통화 음성 (μ-law 8kHz)
GEMINI_OUT_RATE = 24000  # Gemini 음성 출력은 항상 24kHz
DELTA_BYTES = 800        # AI 음성을 0.1초씩 묶어 보낸다 (메시지마다 붙는 JSON 머리말과 ESP32 처리 횟수를 줄인다)


class DeviceSession:
    """
    send        기기로 JSON 메시지를 보내는 코루틴 함수
    messages    기기에서 받은 JSON 메시지(dict)의 비동기 반복자
    client      genai.Client (테스트에서 가짜로 바꿔 끼운다)
    model       Gemini Live 모델 이름
    data_dir    접수 메모(notes.jsonl)와 대화록을 남길 곳
    """

    def __init__(self, send, messages, client: genai.Client, model: str, data_dir: Path, peer: str = "") -> None:
        self._send_json = send
        self._messages = messages
        self._client = client
        self._model = model
        self._data_dir = data_dir
        self._peer = peer
        self._caller = ""
        self._pending_text: list[str] = []
        self._pending_tools: list[types.FunctionResponse] = []
        self._call_names: dict[str, str] = {}
        self._n = 0
        self._response: dict | None = None  # 지금 기기로 보내는 대답 (id, item_id, said, audio, calls, usage)
        self._heard = ""
        self._transcript: list[str] = []
        self._started = datetime.now()

    async def _send(self, msg: dict) -> None:
        await self._send_json(msg)

    def _next_id(self, prefix: str) -> str:
        self._n += 1
        return f"{prefix}_{self._n}"

    # ---- 전체 흐름 ----

    async def run(self) -> None:
        await self._send({"type": "session.created", "session": {"model": self._model}})
        first = await self._next_message()
        if first is None:
            return
        if first.get("type") != "session.update":
            await self._error("session.update 가 먼저 와야 합니다")
            return
        config = self._live_config(first.get("session") or {})
        try:
            async with self._client.aio.live.connect(model=self._model, config=config) as gemini:
                log.info("%s Gemini 세션 시작 (%s)", self._peer, self._model)
                await self._send({"type": "session.updated", "session": {"model": self._model}})
                tasks = [asyncio.create_task(self._from_device(gemini)), asyncio.create_task(self._from_gemini(gemini))]
                done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_COMPLETED)
                for task in pending:
                    task.cancel()
                await asyncio.gather(*pending, return_exceptions=True)
                for task in done:
                    if task.exception() is not None:
                        raise task.exception()
        except Exception as e:  # Gemini 연결 실패·끊김 → 기기는 사람에게 넘긴다
            log.warning("%s Gemini 오류: %r", self._peer, e)
            await self._error(f"Gemini: {e}")
        finally:
            self._save_transcript()
            log.info("%s 세션 끝", self._peer)

    async def _next_message(self) -> dict | None:
        try:
            return await anext(self._messages)
        except StopAsyncIteration:
            return None

    async def _error(self, message: str) -> None:
        try:
            await self._send({"type": "error", "error": {"type": "server_error", "code": "upstream_error", "message": message}})
        except Exception:
            pass

    def _live_config(self, session: dict) -> types.LiveConnectConfig:
        declarations = [
            types.FunctionDeclaration(
                name=t["name"], description=t.get("description", ""),
                parameters_json_schema=t.get("parameters") or {"type": "object", "properties": {}},
            )
            for t in session.get("tools") or []
            if t.get("type") == "function"
        ]
        return types.LiveConnectConfig(
            response_modalities=[types.Modality.AUDIO],
            system_instruction=session.get("instructions") or None,
            tools=[types.Tool(function_declarations=declarations)] if declarations else None,
            input_audio_transcription=types.AudioTranscriptionConfig(),
            output_audio_transcription=types.AudioTranscriptionConfig(),
        )

    # ---- 기기 → Gemini ----

    async def _from_device(self, gemini) -> None:
        async for msg in self._messages:
            kind = msg.get("type")
            if kind == "input_audio_buffer.append":
                pcm = ulaw.decode(base64.b64decode(msg.get("audio", "")))
                await gemini.send_realtime_input(audio=types.Blob(data=pcm.astype("<i2").tobytes(),
                                                                  mime_type=f"audio/pcm;rate={CALL_RATE}"))
            elif kind == "conversation.item.create":
                self._add_item(msg.get("item") or {})
            elif kind == "response.create":
                await self._flush_to_gemini(gemini)
            # conversation.item.truncate, 두 번째 session.update 등은 Gemini 에 해당하는 것이 없다

    def _add_item(self, item: dict) -> None:
        if item.get("type") == "message":
            for part in item.get("content") or []:
                if part.get("type") == "input_text" and part.get("text"):
                    self._pending_text.append(part["text"])
                    if item.get("role") == "system" and "전화번호" in part["text"]:
                        self._caller = part["text"].rsplit(" ", 1)[-1]
        elif item.get("type") == "function_call_output":
            call_id = item.get("call_id", "")
            try:
                result = json.loads(item.get("output") or "{}")
            except json.JSONDecodeError:
                result = {"output": item.get("output")}
            if not isinstance(result, dict):
                result = {"output": result}
            self._pending_tools.append(types.FunctionResponse(id=call_id, name=self._call_names.get(call_id, ""),
                                                              response=result))

    async def _flush_to_gemini(self, gemini) -> None:
        if self._pending_tools:
            await gemini.send_tool_response(function_responses=self._pending_tools)
            self._pending_tools = []
        if self._pending_text:
            text = "\n".join(self._pending_text)
            self._pending_text = []
            await gemini.send_client_content(turns=types.Content(role="user", parts=[types.Part(text=text)]),
                                             turn_complete=True)

    # ---- Gemini → 기기 ----

    async def _from_gemini(self, gemini) -> None:
        while True:
            got_any = False
            async for msg in gemini.receive():  # 모델 한 턴이 끝나면 반복이 끝난다
                got_any = True
                await self._handle(msg)
            if not got_any:
                raise ConnectionError("Gemini 세션이 닫혔습니다")

    async def _begin_response(self) -> dict:
        if self._response is None:
            await self._flush_heard()
            self._response = {"id": self._next_id("resp"), "item_id": self._next_id("item"), "said": "",
                              "audio": False, "buf": bytearray(), "calls": [], "usage": None,
                              "down": soxr.ResampleStream(GEMINI_OUT_RATE, CALL_RATE, 1, dtype="int16")}
            await self._send({"type": "response.created", "response": {"id": self._response["id"], "status": "in_progress"}})
        return self._response

    async def _send_audio(self, resp: dict, pcm8k: np.ndarray, flush: bool = False) -> None:
        resp["buf"] += ulaw.encode(pcm8k) if pcm8k.size else b""
        while len(resp["buf"]) >= DELTA_BYTES or (flush and resp["buf"]):
            chunk = bytes(resp["buf"][:DELTA_BYTES])
            del resp["buf"][:DELTA_BYTES]
            resp["audio"] = True
            await self._send({"type": "response.output_audio.delta", "response_id": resp["id"],
                              "item_id": resp["item_id"], "output_index": 0, "content_index": 0,
                              "delta": base64.b64encode(chunk).decode()})

    async def _handle(self, msg: types.LiveServerMessage) -> None:
        if msg.data:
            resp = await self._begin_response()
            await self._send_audio(resp, resp["down"].resample_chunk(np.frombuffer(msg.data, dtype="<i2")))

        if msg.usage_metadata is not None and self._response is not None:
            self._response["usage"] = msg.usage_metadata

        sc = msg.server_content
        if sc is not None:
            if sc.input_transcription and sc.input_transcription.text:
                self._heard += sc.input_transcription.text
            if sc.output_transcription and sc.output_transcription.text:
                (await self._begin_response())["said"] += sc.output_transcription.text
            if sc.interrupted:
                await self._send({"type": "input_audio_buffer.speech_started",
                                  "item_id": self._response["item_id"] if self._response else ""})
                await self._finish_response("cancelled")
            elif sc.turn_complete:
                await self._finish_response("completed")
                await self._flush_heard()

        if msg.tool_call and msg.tool_call.function_calls:
            resp = await self._begin_response()
            for fc in msg.tool_call.function_calls:
                args = json.dumps(dict(fc.args or {}), ensure_ascii=False)
                self._call_names[fc.id or ""] = fc.name or ""
                resp["calls"].append({"type": "function_call", "name": fc.name, "call_id": fc.id, "arguments": args})
                log.info("%s AI 도구: %s %s", self._peer, fc.name, args)
                if fc.name == "save_note":
                    self._save_note(dict(fc.args or {}))
            await self._finish_response("completed")  # 기기가 도구를 처리한다 (끊기·인계·메모)

        if msg.go_away is not None:
            log.warning("%s Gemini 세션 종료 예고: %s 남음", self._peer, msg.go_away.time_left)

    async def _finish_response(self, status: str) -> None:
        resp = self._response
        if resp is None:
            return
        self._response = None
        if status == "completed":  # 끼어들어 취소된 대답의 남은 음성은 버린다
            await self._send_audio(resp, resp["down"].resample_chunk(np.zeros(0, dtype="<i2"), last=True), flush=True)
        if resp["audio"]:
            await self._send({"type": "response.output_audio.done", "response_id": resp["id"], "item_id": resp["item_id"]})
        if resp["said"]:
            await self._send({"type": "response.output_audio_transcript.done", "response_id": resp["id"],
                              "item_id": resp["item_id"], "transcript": resp["said"]})
            self._transcript.append(f"AI: {resp['said'].strip()}")
            log.info("%s AI: %s", self._peer, resp["said"].strip())
        done = {"id": resp["id"], "status": status, "output": resp["calls"]}
        if resp["usage"] is not None:
            done["usage"] = _usage(resp["usage"])
        await self._send({"type": "response.done", "response": done})

    async def _flush_heard(self) -> None:
        if self._heard.strip():
            text = self._heard.strip()
            self._heard = ""
            self._transcript.append(f"상대: {text}")
            log.info("%s 상대: %s", self._peer, text)
            await self._send({"type": "conversation.item.input_audio_transcription.completed",
                              "item_id": self._next_id("item"), "content_index": 0, "transcript": text})

    # ---- 기록 ----

    def _save_note(self, args: dict) -> None:
        note = {"time": datetime.now().isoformat(timespec="seconds"), "caller": self._caller, **args}
        self._data_dir.mkdir(parents=True, exist_ok=True)
        with open(self._data_dir / "notes.jsonl", "a", encoding="utf-8") as f:
            f.write(json.dumps(note, ensure_ascii=False) + "\n")

    def _save_transcript(self) -> None:
        if not self._transcript:
            return
        self._data_dir.mkdir(parents=True, exist_ok=True)
        name = f"{self._started:%Y%m%d-%H%M%S}_{self._caller or 'unknown'}.txt"
        (self._data_dir / name).write_text("\n".join(self._transcript) + "\n", encoding="utf-8")


def _usage(u: types.UsageMetadata) -> dict:
    """Gemini 사용량 → 펌웨어가 읽는 OpenAI 모양 (입력·출력을 음성·글로 나눈다)"""

    def by_modality(details) -> dict:
        out = {"audio_tokens": 0, "text_tokens": 0}
        for d in details or []:
            key = "audio_tokens" if d.modality == types.MediaModality.AUDIO else "text_tokens"
            out[key] += d.token_count or 0
        return out

    return {
        "input_tokens": u.prompt_token_count or 0,
        "output_tokens": u.response_token_count or 0,
        "input_token_details": {**by_modality(u.prompt_tokens_details), "cached_tokens": u.cached_content_token_count or 0},
        "output_token_details": by_modality(u.response_tokens_details),
    }
