"""통화 음성의 반대편 끝(endpoint).

통화 세션은 endpoint 와 다음 네 가지로만 대화한다.
PC 스피커·마이크(LocalAudio)와 AI 음성 에이전트(gemini.GeminiAgent)가 같은 모양이다.

    start(rate)  통화 음성이 연결됐다. rate 는 통화 샘플레이트(Hz).
    play(pcm)    상대방 목소리 (PCM 16bit LE mono, rate)
    take(n)      우리 쪽 목소리를 정확히 n 바이트 돌려준다 (모자라면 무음으로 채움)
    stop()       통화 음성이 끊겼다.

endpoint 는 통화가 시작될 때마다 endpoint_factory(call, control) 로 새로 만든다.
"""

import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Protocol

import numpy as np


@dataclass(frozen=True)
class CallInfo:
    number: str | None   # 상대 번호 (모르면 None)
    record_base: Path    # 이 통화의 기록 파일 경로 (확장자 없음: .wav 녹음, .txt 대화록)


class CallControl(Protocol):
    """endpoint 가 통화 자체를 조작할 때 쓰는 손잡이."""

    def hangup(self) -> None: ...
    def handover(self) -> None: ...  # 통화는 유지하고 소리만 폰 본체로 넘긴다


class AudioEndpoint(Protocol):
    def start(self, rate: int) -> None: ...
    def play(self, pcm: bytes) -> None: ...
    def take(self, nbytes: int) -> bytes: ...
    def stop(self) -> None: ...


EndpointFactory = Callable[[CallInfo, CallControl], AudioEndpoint]


class PcmFifo:
    """스레드 사이에서 PCM 을 넘기는 버퍼.

    prebuffer 바이트가 쌓이기 전에는 무음을 내보내 지터를 흡수하고,
    max_bytes 를 넘으면 오래된 데이터부터 버려 지연이 쌓이지 않게 한다.
    """

    def __init__(self, max_bytes: int, prebuffer: int = 0) -> None:
        self._buf = bytearray()
        self._lock = threading.Lock()
        self._max = max_bytes
        self._prebuffer = prebuffer
        self._primed = prebuffer == 0

    def write(self, data: bytes) -> None:
        with self._lock:
            self._buf += data
            overflow = len(self._buf) - self._max
            if overflow > 0:
                del self._buf[: overflow + (overflow & 1)]

    def read(self, nbytes: int) -> bytes:
        with self._lock:
            if not self._primed:
                if len(self._buf) < self._prebuffer:
                    return bytes(nbytes)
                self._primed = True
            out = bytes(self._buf[:nbytes])
            del self._buf[:nbytes]
            if len(out) < nbytes:
                self._primed = self._prebuffer == 0
                out += bytes(nbytes - len(out))
            return out

    def pending(self) -> int:
        with self._lock:
            return len(self._buf)

    def clear(self) -> None:
        with self._lock:
            self._buf.clear()
            self._primed = self._prebuffer == 0


class LocalAudio:
    """PC 스피커로 상대방 목소리를 듣고, PC 마이크로 말한다.

    장치는 각자의 기본 샘플레이트로 열고, 통화 샘플레이트와는 soxr 로 변환한다.
    """

    def __init__(self) -> None:
        self._streams: list = []

    def start(self, rate: int) -> None:
        import sounddevice as sd
        import soxr

        in_rate = int(sd.query_devices(kind="input")["default_samplerate"])
        out_rate = int(sd.query_devices(kind="output")["default_samplerate"])

        self._mic_resampler = soxr.ResampleStream(in_rate, rate, 1, dtype="int16")
        self._spk_resampler = soxr.ResampleStream(rate, out_rate, 1, dtype="int16")
        bytes_per_sec_call = rate * 2
        bytes_per_sec_out = out_rate * 2
        self._mic = PcmFifo(max_bytes=bytes_per_sec_call // 2)
        self._speaker = PcmFifo(max_bytes=bytes_per_sec_out // 2, prebuffer=bytes_per_sec_out // 25)  # 40ms

        mic = sd.RawInputStream(samplerate=in_rate, channels=1, dtype="int16", callback=self._on_mic)
        spk = sd.RawOutputStream(samplerate=out_rate, channels=1, dtype="int16", callback=self._on_speaker)
        self._streams = [mic, spk]
        for s in self._streams:
            s.start()

    def _on_mic(self, indata, frames, time, status) -> None:
        pcm = self._mic_resampler.resample_chunk(np.frombuffer(indata, dtype="<i2"))
        self._mic.write(pcm.tobytes())

    def _on_speaker(self, outdata, frames, time, status) -> None:
        outdata[:] = self._speaker.read(len(outdata))

    def play(self, pcm: bytes) -> None:
        out = self._spk_resampler.resample_chunk(np.frombuffer(pcm, dtype="<i2"))
        self._speaker.write(out.tobytes())

    def take(self, nbytes: int) -> bytes:
        return self._mic.read(nbytes)

    def stop(self) -> None:
        for s in self._streams:
            s.stop()
            s.close()
        self._streams = []
