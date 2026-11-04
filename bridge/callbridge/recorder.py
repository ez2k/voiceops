"""통화 녹음 — 스테레오 WAV (왼쪽 = 상대방, 오른쪽 = 우리 쪽)."""

import wave
from pathlib import Path

import numpy as np


class WavRecorder:
    def __init__(self, path: Path, rate: int) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        self.path = path
        self._wav = wave.open(str(path), "wb")
        self._wav.setnchannels(2)
        self._wav.setsampwidth(2)
        self._wav.setframerate(rate)

    def write(self, caller: bytes, ours: bytes) -> None:
        a = np.frombuffer(caller, dtype="<i2")
        b = np.frombuffer(ours, dtype="<i2")
        n = min(len(a), len(b))
        stereo = np.empty(n * 2, dtype="<i2")
        stereo[0::2] = a[:n]
        stereo[1::2] = b[:n]
        self._wav.writeframes(stereo.tobytes())

    def close(self) -> None:
        self._wav.close()
