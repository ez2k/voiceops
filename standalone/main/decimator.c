#include "decimator.h"

#include <math.h>
#include <string.h>

#define IN_RATE     24000.0f
#define CUTOFF_HZ   3400.0f   /* 전화 음성 대역 상한 */
#define FACTOR      3

static float s_coef[DECIMATOR_TAPS];

void decimator_init(void)
{
    const float fc = CUTOFF_HZ / IN_RATE;
    const int m = DECIMATOR_TAPS - 1;
    const float pi = 3.14159265358979f;
    float sum = 0.0f;
    for (int n = 0; n <= m; n++) {
        float x = (float)n - (float)m / 2.0f;
        float sinc = (x == 0.0f) ? 2.0f * fc : sinf(2.0f * pi * fc * x) / (pi * x);
        float blackman = 0.42f - 0.5f * cosf(2.0f * pi * n / m) + 0.08f * cosf(4.0f * pi * n / m);
        s_coef[n] = sinc * blackman;
        sum += s_coef[n];
    }
    for (int n = 0; n <= m; n++) {
        s_coef[n] /= sum;
    }
}

void decimator_reset(decimator_t *d)
{
    memset(d, 0, sizeof(*d));
}

bool decimator_push(decimator_t *d, int16_t in, int16_t *out)
{
    d->hist[d->pos] = (float)in;
    d->pos = (d->pos + 1) % DECIMATOR_TAPS;
    if (++d->phase < FACTOR) {
        return false;
    }
    d->phase = 0;

    float acc = 0.0f;
    int idx = d->pos;  /* 가장 오래된 샘플 */
    for (int n = 0; n < DECIMATOR_TAPS; n++) {
        acc += s_coef[n] * d->hist[idx];
        idx = (idx + 1) % DECIMATOR_TAPS;
    }
    if (acc > 32767.0f) {
        acc = 32767.0f;
    } else if (acc < -32768.0f) {
        acc = -32768.0f;
    }
    *out = (int16_t)lrintf(acc);
    return true;
}
