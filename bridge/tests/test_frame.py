import random

from callbridge import frame


def test_roundtrip_all_types():
    p = frame.Parser()
    frames = [(frame.EVT, b"ring"), (frame.LOG, b"I (12) phone: hi"), (frame.AUDIO, bytes(range(120))), (frame.CMD, b"")]
    data = b"".join(frame.encode(t, pl) for t, pl in frames)
    assert p.feed(data) == frames


def test_garbage_between_frames_and_byte_by_byte():
    p = frame.Parser()
    data = b"\x00boot garbage\xa5" + frame.encode(frame.EVT, b"clip 01012345678") + b"\xa5\xa5\x5a\x01" + frame.encode(frame.EVT, b"call 1")
    got = []
    for b in data:
        got += p.feed(bytes([b]))
    assert got == [(frame.EVT, b"clip 01012345678"), (frame.EVT, b"call 1")]


def test_corrupted_frame_is_dropped_and_next_frame_survives():
    good = frame.encode(frame.EVT, b"audio off")
    bad = bytearray(frame.encode(frame.AUDIO, bytes(60)))
    bad[10] ^= 0xFF
    assert frame.Parser().feed(bytes(bad) + good) == [(frame.EVT, b"audio off")]


def test_oversized_length_resyncs():
    fake_header = b"\xa5\x5a\x03\xff\xff"
    good = frame.encode(frame.EVT, b"ready")
    assert frame.Parser().feed(fake_header + good) == [(frame.EVT, b"ready")]


def test_random_chunking():
    rng = random.Random(1)
    frames = [(frame.AUDIO, bytes(rng.randrange(256) for _ in range(rng.choice([60, 120, 240])))) for _ in range(50)]
    data = b"".join(frame.encode(t, pl) for t, pl in frames)
    p = frame.Parser()
    got = []
    i = 0
    while i < len(data):
        n = rng.randint(1, 300)
        got += p.feed(data[i : i + n])
        i += n
    assert got == frames
