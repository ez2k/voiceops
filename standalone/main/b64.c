#include "b64.h"

static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t b64_encode(const uint8_t *in, size_t len, char *out)
{
    size_t o = 0;
    size_t i = 0;
    for (; i + 2 < len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        out[o++] = ALPHABET[(v >> 6) & 63];
        out[o++] = ALPHABET[v & 63];
    }
    if (i < len) {
        uint32_t v = (uint32_t)in[i] << 16;
        if (i + 1 < len) {
            v |= (uint32_t)in[i + 1] << 8;
        }
        out[o++] = ALPHABET[(v >> 18) & 63];
        out[o++] = ALPHABET[(v >> 12) & 63];
        out[o++] = (i + 1 < len) ? ALPHABET[(v >> 6) & 63] : '=';
        out[o++] = '=';
    }
    return o;
}

void b64_decoder_reset(b64_decoder_t *d)
{
    d->acc = 0;
    d->bits = 0;
}

static int sextet(char c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    }
    if (c >= 'a' && c <= 'z') {
        return c - 'a' + 26;
    }
    if (c >= '0' && c <= '9') {
        return c - '0' + 52;
    }
    if (c == '+' || c == '-') {
        return 62;
    }
    if (c == '/' || c == '_') {
        return 63;
    }
    return -1;
}

int b64_decode_char(b64_decoder_t *d, char c, uint8_t *out)
{
    int v = sextet(c);
    if (v < 0) {
        return 0;
    }
    d->acc = ((d->acc << 6) | (uint32_t)v) & 0xFFFFFF;
    d->bits += 6;
    if (d->bits >= 8) {
        d->bits -= 8;
        *out = (uint8_t)(d->acc >> d->bits);
        return 1;
    }
    return 0;
}
