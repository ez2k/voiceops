/*
 * 단독형 펌웨어의 순수 C 모듈(base64, 다운샘플러, Gemini 메시지 스트리밍 처리기)을 PC 에서 시험한다.
 *   ./run.sh
 */
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "decimator.h"
#include "gemini_messages.h"
#include "gemini_stream.h"

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

/* ---- 24k -> 8k 다운샘플러 ---- */

static double tone_gain(double freq)
{
    decimator_t d;
    decimator_reset(&d);
    const int n_in = 24000;  /* 1초 */
    double peak = 0;
    int outputs = 0;
    for (int i = 0; i < n_in; i++) {
        int16_t x = (int16_t)lrint(10000.0 * sin(2 * M_PI * freq * i / 24000.0));
        int16_t y;
        if (decimator_push(&d, x, &y)) {
            outputs++;
            if (outputs > 100 && abs(y) > peak) {  /* 필터 지연 이후만 */
                peak = abs(y);
            }
        }
    }
    CHECK(outputs == n_in / 3);
    return peak / 10000.0;
}

static void test_decimator(void)
{
    decimator_init();
    double pass = tone_gain(1000);
    double stop = tone_gain(6000);
    double alias = tone_gain(9000);
    printf("decimator gain: 1kHz %.3f, 6kHz %.4f (%.1f dB), 9kHz %.4f (%.1f dB)\n",
           pass, stop, 20 * log10(stop), alias, 20 * log10(alias));
    CHECK(pass > 0.95 && pass < 1.05);
    CHECK(stop < 0.01);   /* -40 dB 이하 */
    CHECK(alias < 0.01);
}

/* ---- Gemini 메시지 스트리밍 처리기 ---- */

static uint8_t s_audio[100000];
static size_t s_audio_len;

static void collect(const uint8_t *b, size_t n, void *ctx)
{
    (void)ctx;
    memcpy(s_audio + s_audio_len, b, n);
    s_audio_len += n;
}

static const char *run_stream(gemini_stream_t *s, const char *msg, unsigned seed)
{
    srand(seed);
    s_audio_len = 0;
    gs_begin(s);
    size_t len = strlen(msg);
    size_t i = 0;
    while (i < len) {
        size_t chunk = 1 + (size_t)(rand() % 700);
        if (chunk > len - i) {
            chunk = len - i;
        }
        gs_feed(s, msg + i, chunk);
        i += chunk;
    }
    return gs_end(s);
}

static void test_stream(void)
{
    static char rest[2048];
    gemini_stream_t s;
    gs_init(&s, rest, sizeof(rest), collect, NULL);

    /* 음성 메시지: 실제 서버 메시지 모양 + 큰 base64 */
    static uint8_t pcm[24000];
    for (size_t i = 0; i < sizeof(pcm); i++) {
        pcm[i] = (uint8_t)(i * 7 + 3);
    }
    static char b64[B64_ENCODED_LEN(sizeof(pcm)) + 1];
    size_t n = b64_encode(pcm, sizeof(pcm), b64);
    b64[n] = '\0';
    static char msg[B64_ENCODED_LEN(sizeof(pcm)) + 512];
    snprintf(msg, sizeof(msg),
             "{\n  \"serverContent\": {\n    \"modelTurn\": {\n      \"parts\": [\n        {\n          \"inlineData\": {\n"
             "            \"mimeType\": \"audio/pcm;rate=24000\",\n            \"data\": \"%s\"\n          }\n        }\n      ]\n    }\n  }\n}\n",
             b64);
    for (unsigned seed = 1; seed <= 5; seed++) {
        const char *r = run_stream(&s, msg, seed);
        CHECK(r != NULL);
        CHECK(s_audio_len == sizeof(pcm) && memcmp(s_audio, pcm, sizeof(pcm)) == 0);
        CHECK(r && strstr(r, "\"data\": \"\"") != NULL);
        CHECK(r && strstr(r, "\"mimeType\": \"audio/pcm;rate=24000\"") != NULL);
        CHECK(r && strlen(r) < 300);
    }

    /* 제어 메시지는 그대로 통과 */
    const char *ctl = "{\"serverContent\":{\"interrupted\":true,\"outputTranscription\":{\"text\":\"안녕하세요 \\\"data\\\": 테스트\"}}}";
    const char *r = run_stream(&s, ctl, 7);
    CHECK(r && strcmp(r, ctl) == 0);
    CHECK(s_audio_len == 0);

    /* 값이 "data" 인 문자열, 도구 호출 */
    const char *tool = "{\"toolCall\":{\"functionCalls\":[{\"id\":\"data\",\"name\":\"save_note\",\"args\":{\"request\":\"data\"}}]}}";
    r = run_stream(&s, tool, 8);
    CHECK(r && strcmp(r, tool) == 0);
    CHECK(s_audio_len == 0);

    /* 여러 data 필드, 슬래시 이스케이프("\/") */
    const char *two = "{\"parts\":[{\"inlineData\":{\"data\":\"AAEC\"}},{\"inlineData\":{\"data\" : \"Aw\\/=\"}}]}";
    r = run_stream(&s, two, 9);
    const uint8_t want[] = {0x00, 0x01, 0x02, 0x03, 0x0f};
    CHECK(s_audio_len == sizeof(want) && memcmp(s_audio, want, sizeof(want)) == 0);
    CHECK(r && strcmp(r, "{\"parts\":[{\"inlineData\":{\"data\":\"\"}},{\"inlineData\":{\"data\" : \"\"}}]}") == 0);

    /* 제어 JSON 이 버퍼보다 크면 NULL */
    static char small[16];
    gemini_stream_t s2;
    gs_init(&s2, small, sizeof(small), collect, NULL);
    CHECK(run_stream(&s2, "{\"setupComplete\":{}, \"x\":\"0123456789\"}", 3) == NULL);
}

/* 펌웨어가 보내는 고정 JSON 을 한 줄씩 출력한다 (run.sh 가 파이썬으로 유효성 검사) */
static void dump_messages(void)
{
    const uint8_t pcm[] = {0x01, 0x02, 0x03, 0x04};
    char b64[B64_ENCODED_LEN(sizeof(pcm)) + 1];
    b64[b64_encode(pcm, sizeof(pcm), b64)] = '\0';
    printf("%s%s%s\n", GEMINI_AUDIO_PREFIX, b64, GEMINI_AUDIO_SUFFIX);
    printf("%s\n", GEMINI_GREETING_JSON);
    printf("%s\n", GEMINI_SPEED_TEST_JSON);
    printf("%s\n", GEMINI_TOOLS_JSON);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "dump") == 0) {
        dump_messages();
        return 0;
    }
    test_b64();
    test_decimator();
    test_stream();
    if (s_failures) {
        printf("%d FAILED\n", s_failures);
        return 1;
    }
    printf("all host tests passed\n");
    return 0;
}
