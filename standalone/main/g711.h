/* G.711 μ-law (전화 음성 8비트 압축). 통화 음성과 OpenAI Realtime 의 audio/pcmu 가 이 형식이다. */
#pragma once

#include <stdint.h>

uint8_t g711_ulaw_encode(int16_t pcm);
int16_t g711_ulaw_decode(uint8_t ulaw);
