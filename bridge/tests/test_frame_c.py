"""펌웨어(C)와 브리지(파이썬)의 프레임 규칙이 같은지 교차 확인한다. C 컴파일러가 없으면 건너뛴다."""

import shutil
import subprocess
from pathlib import Path

import pytest

from callbridge import frame

ROOT = Path(__file__).resolve().parents[2]
FIRMWARE_MAIN = ROOT / "usb-bridge" / "main"


@pytest.fixture(scope="module")
def harness(tmp_path_factory):
    cc = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
    if cc is None:
        pytest.skip("C 컴파일러 없음")
    exe = tmp_path_factory.mktemp("c") / "frame_harness"
    subprocess.run(
        [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-I", str(FIRMWARE_MAIN),
         str(FIRMWARE_MAIN / "frame.c"), str(Path(__file__).with_name("frame_c_harness.c")), "-o", str(exe)],
        check=True,
    )
    return exe


def test_c_encode_matches_python(harness):
    payload = bytes(range(240))
    c_out = subprocess.run([harness, "encode"], input=payload, capture_output=True, check=True).stdout
    assert c_out == frame.encode(frame.AUDIO, payload)


def test_c_parses_python_frames(harness):
    bad = bytearray(frame.encode(frame.AUDIO, b"\x01\x02"))
    bad[-1] ^= 0xFF
    stream = (
        b"garbage\xa5"
        + frame.encode(frame.CMD, b"dial 01012345678")
        + bytes(bad)
        + frame.encode(frame.AUDIO, bytes(range(120)))
        + frame.encode(frame.CMD, b"")
    )
    out = subprocess.run([harness, "parse"], input=stream, capture_output=True, check=True).stdout.decode()
    assert out.splitlines() == [
        f"{frame.CMD} 16 {b'dial 01012345678'.hex()}",
        f"{frame.AUDIO} 120 {bytes(range(120)).hex()}",
        f"{frame.CMD} 0 ",
    ]
