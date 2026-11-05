#include "rt_stream.h"

#include <string.h>

enum {
    S_OUT = 0,      /* 문자열 밖 */
    S_STR,          /* 문자열 안 (키 이름을 모은다) */
    S_STR_ESC,      /* 문자열 안 역슬래시 다음 */
    S_AFTER_KEY,    /* 음성 키 문자열 뒤, ':' 를 기다린다 */
    S_AFTER_COLON,  /* ':' 뒤, 여는 따옴표를 기다린다 */
    S_DATA,         /* base64 음성 */
};

static void rest_put(rt_stream_t *s, char c)
{
    if (s->rest_len + 1 < s->rest_cap) {
        s->rest[s->rest_len++] = c;
    } else {
        s->rest_overflow = true;
    }
}

static void audio_flush(rt_stream_t *s)
{
    if (s->audio_len > 0) {
        s->on_audio(s->audio, s->audio_len, s->ctx);
        s->audio_len = 0;
    }
}

static bool is_space(char c)
{
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

/* 지금까지 모은 JSON 에 only_if 가 나왔는가 (음성 메시지인가) */
static bool rest_contains(const rt_stream_t *s, const char *needle)
{
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= s->rest_len; i++) {
        if (memcmp(s->rest + i, needle, n) == 0) {
            return true;
        }
    }
    return false;
}

static bool is_audio_key(const rt_stream_t *s)
{
    size_t n = strlen(s->audio_key);
    return !s->key_bad && (size_t)s->key_len == n && memcmp(s->key, s->audio_key, n) == 0 &&
           (s->only_if == NULL || rest_contains(s, s->only_if));
}

static void feed_char(rt_stream_t *s, char c)
{
    switch (s->state) {
    case S_OUT:
        rest_put(s, c);
        if (c == '"') {
            s->state = S_STR;
            s->key_len = 0;
            s->key_bad = false;
        }
        return;

    case S_STR:
        rest_put(s, c);
        if (c == '\\') {
            s->key_bad = true;
            s->state = S_STR_ESC;
        } else if (c == '"') {
            s->state = is_audio_key(s) ? S_AFTER_KEY : S_OUT;
        } else if (s->key_len < RS_KEY_MAX) {
            s->key[s->key_len++] = c;
        } else {
            s->key_bad = true;
        }
        return;

    case S_STR_ESC:
        rest_put(s, c);
        s->state = S_STR;
        return;

    case S_AFTER_KEY:
        if (is_space(c)) {
            rest_put(s, c);
        } else if (c == ':') {
            rest_put(s, c);
            s->state = S_AFTER_COLON;
        } else {
            s->state = S_OUT;  /* 키가 아니었다 (값이었다) */
            feed_char(s, c);
        }
        return;

    case S_AFTER_COLON:
        if (is_space(c)) {
            rest_put(s, c);
        } else if (c == '"') {
            rest_put(s, c);
            b64_decoder_reset(&s->b64);
            s->state = S_DATA;
        } else {
            s->state = S_OUT;  /* 문자열이 아닌 값 */
            feed_char(s, c);
        }
        return;

    case S_DATA:
        if (c == '"') {
            audio_flush(s);
            rest_put(s, c);
            s->state = S_OUT;
            return;
        }
        uint8_t byte;
        if (b64_decode_char(&s->b64, c, &byte)) {  /* '\/' 같은 이스케이프의 역슬래시는 무시된다 */
            s->audio[s->audio_len++] = byte;
            if (s->audio_len == sizeof(s->audio)) {
                audio_flush(s);
            }
        }
        return;

    default:
        s->state = S_OUT;
        return;
    }
}

void rs_init(rt_stream_t *s, char *rest_buf, size_t rest_cap, const char *audio_key, const char *only_if,
             rs_audio_cb_t on_audio, void *ctx)
{
    memset(s, 0, sizeof(*s));
    s->rest = rest_buf;
    s->rest_cap = rest_cap;
    s->audio_key = audio_key;
    s->only_if = only_if;
    s->on_audio = on_audio;
    s->ctx = ctx;
}

void rs_begin(rt_stream_t *s)
{
    s->state = S_OUT;
    s->rest_len = 0;
    s->rest_overflow = false;
    s->audio_len = 0;
}

void rs_feed(rt_stream_t *s, const char *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        feed_char(s, data[i]);
    }
    audio_flush(s);
}

const char *rs_end(rt_stream_t *s)
{
    audio_flush(s);
    if (s->rest_overflow) {
        return NULL;
    }
    s->rest[s->rest_len] = '\0';
    return s->rest;
}
