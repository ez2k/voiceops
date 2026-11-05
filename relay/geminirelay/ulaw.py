"""G.711 μ-law ⇄ 16bit PCM — 펌웨어(standalone/main/g711.c)와 같은 계산 (Sun 공개 구현과 같은 방식)."""

import numpy as np

_BIAS = 0x84
_CLIP = 32635


def encode(pcm: np.ndarray) -> bytes:
    """int16 PCM → μ-law 바이트"""
    x = pcm.astype(np.int32)
    sign = np.where(x < 0, 0x80, 0)
    v = np.minimum(np.abs(x), _CLIP) + _BIAS
    exponent = np.clip(np.floor(np.log2(v)).astype(np.int32) - 7, 0, 7)  # 가장 높은 비트 위치 - 7
    mantissa = (v >> (exponent + 3)) & 0x0F
    return (~(sign | (exponent << 4) | mantissa) & 0xFF).astype(np.uint8).tobytes()


def decode(data: bytes) -> np.ndarray:
    """μ-law 바이트 → int16 PCM"""
    u = (~np.frombuffer(data, dtype=np.uint8)).astype(np.int32) & 0xFF
    t = ((u & 0x0F) << 3) + _BIAS
    t = t << ((u & 0x70) >> 4)
    return np.where(u & 0x80, _BIAS - t, t - _BIAS).astype(np.int16)
