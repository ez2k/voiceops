/*
 * 단독형 펌웨어의 순수 C 모듈(base64, G.711 μ-law, Realtime 메시지 스트리밍 처리기)을 PC 에서 시험한다.
 *   ./run.sh
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "g711.h"
#include "realtime_messages.h"
#include "rt_stream.h"

static int s_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

/* ---- base64 ---- */

static void test_b64(void)
{
    const char *cases[][2] = {
        {"", ""}, {"f", "Zg=="}, {"fo", "Zm8="}, {"foo", "Zm9v"}, {"foob", "Zm9vYg=="}, {"fooba", "Zm9vYmE="}, {"foobar", "Zm9vYmFy"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char out[16] = {0};
        size_t n = b64_encode((const uint8_t *)cases[i][0], strlen(cases[i][0]), out);
        CHECK(n == strlen(cases[i][1]) && memcmp(out, cases[i][1], n) == 0);

        b64_decoder_t d;
        b64_decoder_reset(&d);
        uint8_t dec[16];
        size_t m = 0;
        for (const char *c = cases[i][1]; *c; c++) {
            m += b64_decode_char(&d, *c, &dec[m]);
        }
        CHECK(m == strlen(cases[i][0]) && memcmp(dec, cases[i][0], m) == 0);
    }

    uint8_t raw[1000];
    for (int i = 0; i < 1000; i++) {
        raw[i] = (uint8_t)(i * 37 + 11);
    }
    char enc[B64_ENCODED_LEN(1000)];
    size_t n = b64_encode(raw, sizeof(raw), enc);
    b64_decoder_t d;
    b64_decoder_reset(&d);
    uint8_t dec[1000];
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        m += b64_decode_char(&d, enc[i], &dec[m]);
    }
    CHECK(m == 1000 && memcmp(raw, dec, 1000) == 0);
}

/* ---- G.711 μ-law ---- */

static void test_g711(void)
{
    CHECK(g711_ulaw_encode(0) == 0xFF);
    CHECK(g711_ulaw_decode(0xFF) == 0);
    CHECK(g711_ulaw_decode(0x80) == 32124);
    CHECK(g711_ulaw_decode(0x00) == -32124);
    CHECK(g711_ulaw_encode(32767) == 0x80);
    CHECK(g711_ulaw_encode(-32768) == 0x00);

    /* 왕복: 오차는 값 크기에 비례(로그 압축) — 작은 소리는 정밀, 큰 소리는 3% 안 */
    for (int v = -32768; v <= 32767; v += 7) {
        int back = g711_ulaw_decode(g711_ulaw_encode((int16_t)v));
        int err = abs(back - v);
        CHECK(err <= 4 + abs(v) / 32 || abs(v) > 32124);
        if (s_failures > 10) {
            break;
        }
    }
    /* 256개 코드 모두 디코드 → 다시 인코드하면 같은 코드 (0x7F 와 0xFF 는 둘 다 0 이라 0xFF 로) */
    for (int u = 0; u < 256; u++) {
        uint8_t again = g711_ulaw_encode(g711_ulaw_decode((uint8_t)u));
        CHECK(again == u || (u == 0x7F && again == 0xFF));
    }
}

/* ---- Realtime 메시지 스트리밍 처리기 ---- */

static uint8_t s_audio[100000];
static size_t s_audio_len;

static void collect(const uint8_t *b, size_t n, void *ctx)
{
    (void)ctx;
    memcpy(s_audio + s_audio_len, b, n);
    s_audio_len += n;
}

static const char *run_stream(rt_stream_t *s, const char *msg, unsigned seed)
{
    srand(seed);
    s_audio_len = 0;
    rs_begin(s);
    size_t len = strlen(msg);
    size_t i = 0;
    while (i < len) {
        size_t chunk = 1 + (size_t)(rand() % 700);
        if (chunk > len - i) {
            chunk = len - i;
        }
        rs_feed(s, msg + i, chunk);
        i += chunk;
    }
    return rs_end(s);
}

#define AUDIO_TYPE "\"response.output_audio.delta\""

static void test_stream(void)
{
    static char rest[2048];
    rt_stream_t s;
    rs_init(&s, rest, sizeof(rest), "delta", AUDIO_TYPE, collect, NULL);

    /* 음성 메시지: OpenAI Realtime response.output_audio.delta 모양 + 큰 base64 */
    static uint8_t ulaw[24000];
    for (size_t i = 0; i < sizeof(ulaw); i++) {
        ulaw[i] = (uint8_t)(i * 7 + 3);
    }
    static char b64[B64_ENCODED_LEN(sizeof(ulaw)) + 1];
    size_t n = b64_encode(ulaw, sizeof(ulaw), b64);
    b64[n] = '\0';
    static char msg[B64_ENCODED_LEN(sizeof(ulaw)) + 512];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"response.output_audio.delta\",\"event_id\":\"event_abc\",\"response_id\":\"resp_1\","
             "\"item_id\":\"item_42\",\"output_index\":0,\"content_index\":0,\"delta\":\"%s\"}",
             b64);
    for (unsigned seed = 1; seed <= 5; seed++) {
        const char *r = run_stream(&s, msg, seed);
        CHECK(r != NULL);
        CHECK(s_audio_len == sizeof(ulaw) && memcmp(s_audio, ulaw, sizeof(ulaw)) == 0);
        CHECK(r && strstr(r, "\"delta\":\"\"") != NULL);
        CHECK(r && strstr(r, "\"item_id\":\"item_42\"") != NULL);
        CHECK(r && strlen(r) < 300);
    }

    /* 자막 조각도 "delta" 를 쓰지만 음성이 아니다 → 그대로 통과 */
    const char *transcript = "{\"type\":\"response.output_audio_transcript.delta\",\"item_id\":\"item_42\",\"delta\":\"안녕하세요 \\\"delta\\\" 테스트\"}";
    const char *r = run_stream(&s, transcript, 7);
    CHECK(r && strcmp(r, transcript) == 0);
    CHECK(s_audio_len == 0);

    /* 도구 호출이 담긴 response.done, 값이 "delta" 인 문자열 */
    const char *done = "{\"type\":\"response.done\",\"response\":{\"output\":[{\"type\":\"function_call\",\"name\":\"save_note\","
                       "\"call_id\":\"call_1\",\"arguments\":\"{\\\"request\\\":\\\"delta\\\"}\"}],\"note\":\"delta\"}}";
    r = run_stream(&s, done, 8);
    CHECK(r && strcmp(r, done) == 0);
    CHECK(s_audio_len == 0);

    /* 공백, 슬래시 이스케이프("\/") */
    const char *esc = "{\"type\" : \"response.output_audio.delta\", \"delta\" : \"AAEC\\/w==\"}";
    r = run_stream(&s, esc, 9);
    const uint8_t want[] = {0x00, 0x01, 0x02, 0xff};
    CHECK(s_audio_len == sizeof(want) && memcmp(s_audio, want, sizeof(want)) == 0);
    CHECK(r && strcmp(r, "{\"type\" : \"response.output_audio.delta\", \"delta\" : \"\"}") == 0);

    /* 제어 JSON 이 버퍼보다 크면 NULL */
    static char small[16];
    rt_stream_t s2;
    rs_init(&s2, small, sizeof(small), "delta", AUDIO_TYPE, collect, NULL);
    CHECK(run_stream(&s2, "{\"type\":\"session.updated\",\"x\":\"0123456789\"}", 3) == NULL);
}

/* 펌웨어가 보내는 고정 JSON 을 한 줄씩 출력한다 (run.sh 가 파이썬으로 유효성 검사) */
static void dump_messages(void)
{
    const uint8_t ulaw[] = {0x01, 0x02, 0x03, 0x04};
    char b64[B64_ENCODED_LEN(sizeof(ulaw)) + 1];
    b64[b64_encode(ulaw, sizeof(ulaw), b64)] = '\0';
    printf("%s%s%s\n", RT_AUDIO_PREFIX, b64, RT_AUDIO_SUFFIX);
    printf("%s\n", RT_GREETING_JSON);
    printf("%s\n", RT_SPEED_TEST_JSON);
    printf("%s\n", RT_RESPONSE_CREATE_JSON);
    printf("%s\n", RT_TOOLS_JSON);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        dump_messages();
        return 0;
    }
    test_b64();
    test_g711();
    test_stream();
    if (s_failures) {
        printf("%d FAILED\n", s_failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
