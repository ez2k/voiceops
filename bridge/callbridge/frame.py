"""시리얼 프레임 코덱 — firmware/main/frame.c 와 같은 규칙.

    A5 5A | type(1) | len(2, LE) | payload | crc16(2, LE)

crc16 은 CRC-16/CCITT-FALSE(poly 0x1021, init 0xFFFF)이고 type·len·payload 에 대해 계산한다.
"""

import binascii

MAGIC = b"\xa5\x5a"
MAX_PAYLOAD = 1024

EVT = 0x01    # ESP32 -> PC : 상태 이벤트 (텍스트)
LOG = 0x02    # ESP32 -> PC : 로그 (텍스트)
AUDIO = 0x03  # 양방향      : PCM 16bit LE mono
CMD = 0x10    # PC -> ESP32 : 명령 (텍스트)


def crc16(data: bytes) -> int:
    return binascii.crc_hqx(data, 0xFFFF)


def encode(ftype: int, payload: bytes) -> bytes:
    if len(payload) > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {len(payload)}")
    body = bytes([ftype]) + len(payload).to_bytes(2, "little") + payload
    return MAGIC + body + crc16(body).to_bytes(2, "little")


class Parser:
    """바이트 스트림에서 프레임을 꺼낸다. 프레임 밖의 바이트와 깨진 프레임은 버린다."""

    def __init__(self) -> None:
        self._buf = bytearray()

    def feed(self, data: bytes) -> list[tuple[int, bytes]]:
        buf = self._buf
        buf += data
        frames = []
        while True:
            start = buf.find(MAGIC)
            if start < 0:
                # 마지막 바이트가 MAGIC 의 앞 절반일 수 있으니 남겨 둔다.
                del buf[: max(len(buf) - 1, 0)]
                break
            del buf[:start]
            if len(buf) < 5:
                break
            length = int.from_bytes(buf[3:5], "little")
            if length > MAX_PAYLOAD:
                del buf[:1]
                continue
            total = 7 + length
            if len(buf) < total:
                break
            body = bytes(buf[2 : 5 + length])
            crc = int.from_bytes(buf[5 + length : total], "little")
            if crc16(body) != crc:
                del buf[:1]
                continue
            frames.append((buf[2], bytes(buf[5 : 5 + length])))
            del buf[:total]
        return frames
