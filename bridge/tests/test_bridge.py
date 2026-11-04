import wave

import numpy as np

from callbridge import frame
from callbridge.app import Bridge
from callbridge.audio import PcmFifo


class FakeLink:
    def __init__(self):
        self.commands = []
        self.audio = []

    def send_command(self, text):
        self.commands.append(text)

    def send_audio(self, pcm):
        self.audio.append(pcm)


class FakeEndpoint:
    """상대 목소리를 기록하고, 우리 쪽 목소리로 일정한 값(1000)을 낸다."""

    instances = []

    def __init__(self, call=None, control=None):
        self.call = call
        self.control = control
        self.rate = None
        self.played = b""
        self.stopped = False
        FakeEndpoint.instances.append(self)

    def start(self, rate):
        self.rate = rate

    def play(self, pcm):
        self.played += pcm

    def take(self, nbytes):
        return np.full(nbytes // 2, 1000, dtype="<i2").tobytes()

    def stop(self):
        self.stopped = True


def evt(text):
    return frame.EVT, text.encode()


def make_bridge(tmp_path, auto_answer=0):
    link = FakeLink()
    FakeEndpoint.instances.clear()
    bridge = Bridge(link, FakeEndpoint, tmp_path, auto_answer, out=lambda s: None)
    return bridge, link


def test_auto_answer_after_n_rings_only_once(tmp_path):
    bridge, link = make_bridge(tmp_path, auto_answer=2)
    for e in ["callsetup 1", "clip 01012345678", "ring", "ring", "ring"]:
        bridge.handle_frame(*evt(e))
    assert link.commands == ["answer"]


def test_no_auto_answer_by_default(tmp_path):
    bridge, link = make_bridge(tmp_path)
    for e in ["callsetup 1", "ring", "ring", "ring"]:
        bridge.handle_frame(*evt(e))
    assert link.commands == []


def test_call_audio_roundtrip_and_recording(tmp_path):
    bridge, link = make_bridge(tmp_path)
    bridge.handle_frame(*evt("clip 010-1234-5678"))
    bridge.handle_frame(*evt("call 1"))
    bridge.handle_frame(*evt("audio on cvsd 8000 120"))
    ep = FakeEndpoint.instances[-1]
    assert ep.rate == 8000
    assert ep.call.number == "01012345678"
    assert ep.call.record_base.name.endswith("_01012345678")
    assert ep.control is bridge

    caller = np.arange(60, dtype="<i2").tobytes()  # 프레임 하나 = 120바이트
    for _ in range(10):
        bridge.handle_frame(frame.AUDIO, caller)

    # 받은 프레임마다 같은 길이로 하나씩 돌려보낸다
    assert [len(a) for a in link.audio] == [120] * 10
    assert ep.played == caller * 10

    bridge.handle_frame(*evt("audio off"))
    assert ep.stopped

    files = list(tmp_path.glob("*_01012345678.wav"))
    assert len(files) == 1
    with wave.open(str(files[0])) as w:
        assert (w.getnchannels(), w.getframerate(), w.getnframes()) == (2, 8000, 600)
        data = np.frombuffer(w.readframes(600), dtype="<i2")
    assert np.array_equal(data[0::2], np.tile(np.arange(60), 10))  # 왼쪽 = 상대방
    assert np.all(data[1::2] == 1000)                             # 오른쪽 = 우리 쪽


def test_audio_without_session_is_ignored(tmp_path):
    bridge, link = make_bridge(tmp_path)
    bridge.handle_frame(frame.AUDIO, bytes(120))
    assert link.audio == []


def test_dial_names_recording(tmp_path):
    bridge, link = make_bridge(tmp_path)
    bridge.command("dial 0212345678")
    bridge.handle_frame(*evt("audio on cvsd 8000 120"))
    bridge.handle_frame(*evt("call 1"))
    bridge.close()
    assert link.commands == ["dial 0212345678"]
    assert list(tmp_path.glob("*_0212345678.wav"))


def test_inband_ringtone_does_not_start_session_before_answer(tmp_path):
    bridge, link = make_bridge(tmp_path, auto_answer=1)
    bridge.handle_frame(*evt("callsetup 1"))
    bridge.handle_frame(*evt("audio on cvsd 8000 120"))  # 벨소리용 음성 채널
    bridge.handle_frame(frame.AUDIO, bytes(120))
    assert FakeEndpoint.instances == [] and link.audio == []
    bridge.handle_frame(*evt("ring"))
    assert link.commands == ["answer"]
    bridge.handle_frame(*evt("call 1"))                   # 받아서 연결됨 → 이미 열린 채널로 세션 시작
    assert len(FakeEndpoint.instances) == 1
    bridge.handle_frame(frame.AUDIO, bytes(120))
    assert len(link.audio) == 1
    bridge.handle_frame(*evt("call 0"))
    assert FakeEndpoint.instances[0].stopped


def test_call_control_sends_device_commands(tmp_path):
    bridge, link = make_bridge(tmp_path)
    bridge.hangup()
    bridge.handover()
    assert link.commands == ["hangup", "audio off"]


def test_fifo_prebuffer_and_overflow():
    f = PcmFifo(max_bytes=8, prebuffer=4)
    f.write(b"\x01\x01")
    assert f.read(2) == b"\x00\x00"      # 아직 prebuffer 미달 → 무음
    f.write(b"\x02\x02")
    assert f.read(2) == b"\x01\x01"      # 4바이트 쌓여서 재생 시작
    f.write(b"\x03\x03\x04\x04\x05\x05\x06\x06")
    assert f.read(8) == b"\x03\x03\x04\x04\x05\x05\x06\x06"  # 넘친 오래된 데이터(02 02)는 버림
    assert f.read(2) == b"\x00\x00"      # 바닥나면 무음
