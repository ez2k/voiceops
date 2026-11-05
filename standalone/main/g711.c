/* ITU-T G.711 μ-law — 널리 쓰이는 Sun Microsystems 공개 구현(g711.c)과 같은 방식 */
#include "g711.h"

#define ULAW_BIAS 0x84    /* 132 */
#define ULAW_CLIP 32635

uint8_t g711_ulaw_encode(int16_t pcm)
{
    int v = pcm;
    int sign = 0;
    if (v < 0) {
        v = -v;
        sign = 0x80;
    }
    if (v > ULAW_CLIP) {
        v = ULAW_CLIP;
    }
    v += ULAW_BIAS;

    int exponent = 7;
    for (int mask = 0x4000; (v & mask) == 0 && exponent > 0; mask >>= 1) {
        exponent--;
    }
    int mantissa = (v >> (exponent + 3)) & 0x0F;
    return (uint8_t)~(sign | (exponent << 4) | mantissa);
}

int16_t g711_ulaw_decode(uint8_t ulaw)
{
    ulaw = (uint8_t)~ulaw;
    int t = ((ulaw & 0x0F) << 3) + ULAW_BIAS;
    t <<= (ulaw & 0x70) >> 4;
    return (int16_t)((ulaw & 0x80) ? (ULAW_BIAS - t) : (t - ULAW_BIAS));
}
