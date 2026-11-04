"""통화 음성 세션.

폰에서 음성 프레임이 하나 올 때마다, 같은 길이의 우리 쪽 음성을 하나 돌려보낸다.
송신 속도를 수신 프레임에 맞추므로 PC 시계와 블루투스 시계가 어긋나도 지연이 쌓이지 않는다.
"""

from typing import Protocol

from .audio import AudioEndpoint
from .recorder import WavRecorder


class AudioSink(Protocol):
    def send_audio(self, pcm: bytes) -> None: ...


class CallSession:
    def __init__(self, link: AudioSink, endpoint: AudioEndpoint, recorder: WavRecorder, rate: int) -> None:
        self._link = link
        self._endpoint = endpoint
        self._recorder = recorder
        self._endpoint.start(rate)

    def on_caller_audio(self, pcm: bytes) -> None:
        ours = self._endpoint.take(len(pcm))
        self._link.send_audio(ours)
        self._endpoint.play(pcm)
        self._recorder.write(pcm, ours)

    def close(self) -> None:
        self._endpoint.stop()
        self._recorder.close()
