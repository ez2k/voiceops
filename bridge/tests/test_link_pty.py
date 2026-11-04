"""가상 시리얼(pty)로 ESP32 를 흉내 내 Link + Bridge 를 끝에서 끝까지 돌려 본다 (리눅스·맥 전용)."""

import os
import sys
import time

import pytest

from callbridge import frame
from callbridge.app import Bridge
from callbridge.link import Link

from test_bridge import FakeEndpoint

pytestmark = pytest.mark.skipif(sys.platform == "win32", reason="pty 없음")


def read_frames(fd, parser, want, timeout=2.0):
    got = []
    deadline = time.time() + timeout
    while len(got) < want and time.time() < deadline:
        try:
            got += parser.feed(os.read(fd, 4096))
        except BlockingIOError:
            time.sleep(0.01)
    return got


def test_end_to_end_over_pty(tmp_path):
    master, slave = os.openpty()
    os.set_blocking(master, False)
    link = Link(os.ttyname(slave))
    bridge = Bridge(link, FakeEndpoint, tmp_path, auto_answer=1, out=lambda s: None)
    link.start(bridge.handle_frame)
    try:
        device = frame.Parser()
        os.write(master, b"boot noise" + frame.encode(frame.EVT, b"callsetup 1") + frame.encode(frame.EVT, b"ring"))
        assert read_frames(master, device, 1) == [(frame.CMD, b"answer")]

        os.write(master, frame.encode(frame.EVT, b"audio on cvsd 8000 120"))
        time.sleep(0.1)
        for _ in range(5):
            os.write(master, frame.encode(frame.AUDIO, bytes(120)))
        replies = read_frames(master, device, 5)
        assert [(t, len(p)) for t, p in replies] == [(frame.AUDIO, 120)] * 5
    finally:
        link.close()
        bridge.close()
        os.close(master)
        os.close(slave)
