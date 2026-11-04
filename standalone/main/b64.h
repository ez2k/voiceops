/* base64 — 인코딩은 한 번에, 디코딩은 한 글자씩 흘려 넣는다 (큰 메시지를 통째로 담지 않기 위해). */
#pragma once

#include <stddef.h>
#include <stdint.h>

#define B64_ENCODED_LEN(n) ((((n) + 2) / 3) * 4)

/* out 에 B64_ENCODED_LEN(len) 글자를 쓰고 그 길이를 돌려준다 (NUL 은 붙이지 않는다). */
size_t b64_encode(const uint8_t *in, size_t len, char *out);

typedef struct {
    uint32_t acc;
    int bits;
} b64_decoder_t;

void b64_decoder_reset(b64_decoder_t *d);

/* 글자 하나를 넣는다. 바이트가 하나 완성되면 *out 에 쓰고 1, 아니면 0.
 * 알파벳 밖 글자('=', 공백, 역슬래시 등)는 무시한다. */
int b64_decode_char(b64_decoder_t *d, char c, uint8_t *out);
