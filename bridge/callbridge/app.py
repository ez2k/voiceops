"""ESP32 이벤트를 받아 통화를 처리하는 브리지 본체."""

import re
from datetime import datetime
from pathlib import Path
from typing import Callable, Protocol

from . import frame
from .audio import CallInfo, EndpointFactory
from .call import CallSession
from .recorder import WavRecorder


class DeviceLink(Protocol):
    def send_command(self, text: str) -> None: ...
    def send_audio(self, pcm: bytes) -> None: ...


class Bridge:
    """
    link             ESP32 로 명령·음성을 보낼 곳
    endpoint_factory 통화 음성이 연결될 때마다 새 endpoint 를 만든다 (PC 스피커·마이크 또는 AI)
    record_dir       통화 기록(녹음 WAV, 대화록 TXT)을 저장할 폴더
    auto_answer      벨이 이 횟수만큼 울리면 자동으로 받는다 (0 이면 끔)
    out              화면 출력 함수

    Bridge 자신이 endpoint 에게 CallControl(끊기·인계)로 주어진다.
    """

    def __init__(
        self,
        link: DeviceLink,
        endpoint_factory: EndpointFactory,
        record_dir: Path,
        auto_answer: int = 0,
        out: Callable[[str], None] = print,
    ) -> None:
        self._link = link
        self._endpoint_factory = endpoint_factory
        self._record_dir = record_dir
        self._auto_answer = auto_answer
        self._out = out
        self._number: str | None = None
        self._rings = 0
        self._answer_sent = False
        self._session: CallSession | None = None

    # ---- ESP32 -> PC (링크 수신 스레드에서 순서대로 불린다) ----

    def handle_frame(self, ftype: int, payload: bytes) -> None:
        if ftype == frame.AUDIO:
            if self._session is not None:
                self._session.on_caller_audio(payload)
        elif ftype == frame.EVT:
            text = payload.decode("utf-8", "replace")
            self._out(f"[폰] {text}")
            self._on_event(text)
        elif ftype == frame.LOG:
            self._out(f"  · {payload.decode('utf-8', 'replace')}")

    def _on_event(self, text: str) -> None:
        kind, _, rest = text.partition(" ")
        if kind == "callsetup":
            if rest == "1":  # 전화가 걸려오기 시작
                self._rings = 0
                self._answer_sent = False
        elif kind == "clip":
            self._number = rest or None
        elif kind == "ring":
            self._rings += 1
            if self._auto_answer and self._rings >= self._auto_answer and not self._answer_sent:
                self._answer_sent = True
                self._out("[자동응답] 전화를 받습니다")
                self._link.send_command("answer")
        elif kind == "audio":
            args = rest.split()
            if args[:1] == ["on"]:
                self._start_session(rate=int(args[2]))
            elif args[:1] == ["off"]:
                self._stop_session()
        elif kind == "call" and rest == "0":
            self._number = None

    def _start_session(self, rate: int) -> None:
        self._stop_session()
        number = re.sub(r"[^0-9+]", "", self._number or "") or None
        call = CallInfo(number, self._record_dir / f"{datetime.now():%Y%m%d-%H%M%S}_{number or 'unknown'}")
        recorder = WavRecorder(call.record_base.with_suffix(".wav"), rate)
        self._session = CallSession(self._link, self._endpoint_factory(call, self), recorder, rate)
        self._out(f"[통화 음성 연결] {rate}Hz, 기록: {call.record_base}.*")

    def _stop_session(self) -> None:
        if self._session is not None:
            self._session.close()
            self._session = None
            self._out("[통화 음성 끊김] 녹음 저장 완료")

    # ---- endpoint(AI) -> ESP32 : CallControl ----

    def hangup(self) -> None:
        self._out("[통화 종료 요청]")
        self._link.send_command("hangup")

    def handover(self) -> None:
        self._out("[사람에게 인계] 폰 본체로 통화를 넘깁니다 — 폰을 받아 주세요")
        self._link.send_command("audio off")

    # ---- 사용자 -> ESP32 ----

    def command(self, line: str) -> None:
        if line.startswith("dial "):
            self._number = line[5:].strip()
        self._link.send_command(line)

    def close(self) -> None:
        self._stop_session()
