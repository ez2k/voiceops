"""ESP32 와의 USB 시리얼 링크."""

import threading
from typing import Callable

import serial

from . import frame

BAUD_RATE = 921600  # firmware/main/link.h 의 LINK_BAUD_RATE 와 같아야 한다

FrameHandler = Callable[[int, bytes], None]


class Link:
    def __init__(self, port: str) -> None:
        ser = serial.Serial()
        ser.port = port
        ser.baudrate = BAUD_RATE
        ser.timeout = 0.05
        # 개발보드의 자동 리셋 회로가 동작하지 않도록 DTR/RTS 를 내린 채로 연다.
        ser.dtr = False
        ser.rts = False
        ser.open()
        self._ser = ser
        self._write_lock = threading.Lock()
        self._running = False
        self._thread: threading.Thread | None = None

    def start(self, on_frame: FrameHandler) -> None:
        """수신 스레드를 시작한다. on_frame 은 이 스레드에서 프레임 순서대로 불린다."""
        self._running = True
        self._thread = threading.Thread(target=self._read_loop, args=(on_frame,), daemon=True)
        self._thread.start()

    def _read_loop(self, on_frame: FrameHandler) -> None:
        parser = frame.Parser()
        while self._running:
            data = self._ser.read(4096)
            if not data:
                continue
            for ftype, payload in parser.feed(data):
                on_frame(ftype, payload)

    def _write(self, data: bytes) -> None:
        with self._write_lock:
            self._ser.write(data)

    def send_command(self, text: str) -> None:
        self._write(frame.encode(frame.CMD, text.encode("utf-8")))

    def send_audio(self, pcm: bytes) -> None:
        self._write(frame.encode(frame.AUDIO, pcm))

    def close(self) -> None:
        self._running = False
        if self._thread is not None:
            self._thread.join(timeout=1)
        self._ser.close()
