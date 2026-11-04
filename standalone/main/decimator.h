/* 24kHz -> 8kHz 다운샘플러 (저역통과 FIR + 1/3 솎아내기).
 * Gemini 의 출력 음성(24kHz)을 블루투스 통화(CVSD 8kHz)에 맞춘다. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define DECIMATOR_TAPS 47

typedef struct {
    float hist[DECIMATOR_TAPS];
    int pos;
    int phase;
} decimator_t;

/* 필터 계수를 계산한다. 처음 한 번만 부른다. */
void decimator_init(void);
void decimator_reset(decimator_t *d);

/* 24kHz 샘플 하나를 넣는다. 8kHz 샘플이 하나 나오면 *out 에 쓰고 true. */
bool decimator_push(decimator_t *d, int16_t in, int16_t *out);
