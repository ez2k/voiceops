"""μ-law: 알려진 값, 왕복, 그리고 펌웨어 C 구현(standalone/main/g711.c)과 65536 개 값 전부 비교."""

import ctypes
import shutil
import subprocess
from pathlib import Path

import numpy as np
import pytest

from geminirelay import ulaw

G711_C = Path(__file__).resolve().parents[2] / "standalone" / "main" / "g711.c"


def test_known_values():
    assert ulaw.encode(np.array([0, 32767, -32768], np.int16)) == bytes([0xFF, 0x80, 0x00])
    assert list(ulaw.decode(bytes([0xFF, 0x80, 0x00]))) == [0, 32124, -32124]


def test_all_codes_roundtrip():
    codes = bytes(range(256))
    again = ulaw.encode(ulaw.decode(codes))
    assert all(a == b or (b == 0x7F and a == 0xFF) for a, b in zip(again, codes))


@pytest.mark.skipif(shutil.which("cc") is None, reason="C 컴파일러 없음")
def test_matches_firmware(tmp_path):
    lib = tmp_path / "libg711.so"
    subprocess.run(["cc", "-shared", "-fPIC", "-O2", "-I", str(G711_C.parent), str(G711_C), "-o", str(lib)], check=True)
    c = ctypes.CDLL(str(lib))
    c.g711_ulaw_encode.argtypes, c.g711_ulaw_encode.restype = [ctypes.c_int16], ctypes.c_uint8
    c.g711_ulaw_decode.argtypes, c.g711_ulaw_decode.restype = [ctypes.c_uint8], ctypes.c_int16

    pcm = np.arange(-32768, 32768, dtype=np.int32).astype(np.int16)
    ours = np.frombuffer(ulaw.encode(pcm), dtype=np.uint8)
    theirs = np.array([c.g711_ulaw_encode(int(v)) for v in pcm], dtype=np.uint8)
    assert np.array_equal(ours, theirs)
    assert list(ulaw.decode(bytes(range(256)))) == [c.g711_ulaw_decode(u) for u in range(256)]
