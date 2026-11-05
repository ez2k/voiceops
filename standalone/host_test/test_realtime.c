/*
 * realtime.c (OpenAI Realtime 프로토콜) 를 가짜 WebSocket 으로 PC 에서 시험한다.
 * 서버 메시지 모양은 OpenAI 문서(Realtime conversations 가이드)의 예시를 따른다.
 *   ./run.sh
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "b64.h"
#include "cJSON.h"
#include "esp_websocket_client.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "g711.h"
#include "realtime.h"

static int s_failures;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); s_failures++; } } while (0)

/* ---- 가짜 ESP-IDF ---- */

const char test_roots_pem[] __asm__("_binary_server_roots_pem_start") = "-----BEGIN CERTIFICATE-----";

const char *esp_err_to_name(esp_err_t code) { return code == ESP_OK ? "ESP_OK" : "ESP_ERR"; }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (SemaphoreHandle_t)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t w) { (void)s; (void)w; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void)s; return pdTRUE; }
void vTaskDelay(TickType_t t) { (void)t; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *n, uint32_t st, void *a, int p, void *h)
{
    (void)fn; (void)n; (void)st; (void)a; (void)p; (void)h;
    return pdPASS;  /* 송신 태스크는 돌리지 않는다 — 보내는 내용은 상대 음성 대기열에서 확인 */
}

static uint8_t s_caller_q[4096];
static size_t s_caller_q_len;
StreamBufferHandle_t xStreamBufferCreate(size_t size, size_t trigger) { (void)size; (void)trigger; return (StreamBufferHandle_t)2; }
size_t xStreamBufferSend(StreamBufferHandle_t b, const void *d, size_t len, TickType_t w)
{
    (void)b; (void)w;
    memcpy(s_caller_q + s_caller_q_len, d, len);
    s_caller_q_len += len;
    return len;
}
size_t xStreamBufferReceive(StreamBufferHandle_t b, void *d, size_t len, TickType_t w) { (void)b; (void)d; (void)len; (void)w; return 0; }

static char s_uri[256], s_headers[512];
static esp_event_handler_t s_handler;
static struct esp_websocket_client { int unused; } s_client;
static char *s_sent[64];
static int s_sent_n;

esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *c)
{
    snprintf(s_uri, sizeof(s_uri), "%s", c->uri);
    snprintf(s_headers, sizeof(s_headers), "%s", c->headers ? c->headers : "");
    return &s_client;
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t c, esp_websocket_event_id_t id,
                                        esp_event_handler_t h, void *a)
{
    (void)c; (void)id; (void)a;
    s_handler = h;
    return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t c) { (void)c; return ESP_OK; }
esp_err_t esp_websocket_client_close(esp_websocket_client_handle_t c, TickType_t t) { (void)c; (void)t; return ESP_OK; }
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t c) { (void)c; return ESP_OK; }
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t c) { (void)c; return true; }
int esp_websocket_client_send_text(esp_websocket_client_handle_t c, const char *d, int len, TickType_t t)
{
    (void)c; (void)t;
    if (s_sent_n < 64) {
        s_sent[s_sent_n++] = strndup(d, (size_t)len);
    }
    return len;
}

/* ---- 도우미 ---- */

static void clear_sent(void)
{
    for (int i = 0; i < s_sent_n; i++) {
        free(s_sent[i]);
    }
    s_sent_n = 0;
}

static cJSON *sent_json(int i)
{
    return i < s_sent_n ? cJSON_Parse(s_sent[i]) : NULL;
}

static const char *jstr(const cJSON *o, const char *path)
{
    /* "a.b.c" 경로로 문자열 값을 찾는다 */
    char buf[128];
    snprintf(buf, sizeof(buf), "%s", path);
    const cJSON *cur = o;
    for (char *tok = strtok(buf, "."); tok && cur; tok = strtok(NULL, ".")) {
        cur = cJSON_GetObjectItemCaseSensitive(cur, tok);
    }
    return cJSON_GetStringValue(cur);
}

static void fire(int32_t id, esp_websocket_event_data_t *d)
{
    s_handler(NULL, "WEBSOCKET_EVENTS", id, d);
}

/* 서버 메시지 하나를 WebSocket 한 프레임으로, 2048 바이트씩 나눠 넣는다 (실제 클라이언트와 같은 방식) */
static void server_says(const char *json)
{
    int total = (int)strlen(json);
    for (int off = 0; off < total; off += 2048) {
        int n = total - off < 2048 ? total - off : 2048;
        esp_websocket_event_data_t d = {
            .data_ptr = json + off, .data_len = n, .fin = true, .op_code = WS_TRANSPORT_OPCODES_TEXT,
            .client = &s_client, .payload_len = total, .payload_offset = off,
        };
        fire(WEBSOCKET_EVENT_DATA, &d);
    }
}

static char *audio_delta(const char *item_id, const uint8_t *ulaw, size_t n)
{
    char *b64 = malloc(B64_ENCODED_LEN(n) + 1);
    b64[b64_encode(ulaw, n, b64)] = '\0';
    char *msg = malloc(strlen(b64) + 256);
    sprintf(msg, "{\"type\":\"response.output_audio.delta\",\"event_id\":\"event_1\",\"response_id\":\"resp_1\","
                 "\"item_id\":\"%s\",\"output_index\":0,\"content_index\":0,\"delta\":\"%s\"}", item_id, b64);
    free(b64);
    return msg;
}

static int s_end_calls, s_transfers, s_fails;
static char s_transfer_reason[64];
static void on_end_call(void) { s_end_calls++; }
static void on_transfer(const char *reason) { s_transfers++; snprintf(s_transfer_reason, sizeof(s_transfer_reason), "%s", reason); }
static void on_failed(const char *why) { (void)why; s_fails++; }

/* ---- 시험 ---- */

static void test_call(void)
{
    const realtime_callbacks_t cb = {.on_end_call = on_end_call, .on_transfer = on_transfer, .on_failed = on_failed};
    const realtime_config_t cfg = {.url = "wss://api.openai.com/v1/realtime", .api_key = "sk-test", .model = "gpt-realtime-2.1-mini",
                                   .prompt = "# 역할\n\"OO회사\" AI 직원입니다.\n"};
    CHECK(realtime_init(&cb, &cfg) == ESP_OK);
    CHECK(realtime_start("01012345678", RT_OPEN_GREETING) == ESP_OK);
    CHECK(strcmp(s_uri, "wss://api.openai.com/v1/realtime?model=gpt-realtime-2.1-mini") == 0);
    CHECK(strcmp(s_headers, "Authorization: Bearer sk-test\r\n") == 0);

    /* 연결되면 session.update */
    esp_websocket_event_data_t conn = {.client = &s_client};
    fire(WEBSOCKET_EVENT_CONNECTED, &conn);
    CHECK(s_sent_n == 1);
    cJSON *su = sent_json(0);
    CHECK(su != NULL);
    CHECK(strcmp(jstr(su, "type"), "session.update") == 0);
    CHECK(strcmp(jstr(su, "session.type"), "realtime") == 0);
    CHECK(strcmp(jstr(su, "session.instructions"), cfg.prompt) == 0);
    CHECK(strcmp(jstr(su, "session.audio.input.format.type"), "audio/pcmu") == 0);
    CHECK(strcmp(jstr(su, "session.audio.output.format.type"), "audio/pcmu") == 0);
    CHECK(strcmp(jstr(su, "session.audio.output.voice"), "marin") == 0);
    CHECK(strcmp(jstr(su, "session.audio.input.turn_detection.type"), "server_vad") == 0);
    CHECK(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetObjectItem(su, "session"), "tools")) == 3);
    cJSON_Delete(su);
    clear_sent();

    /* 세션이 준비되면: 발신 번호(시스템 메시지) → 첫 인사 신호 → 대답 요청 */
    server_says("{\"type\":\"session.created\",\"session\":{\"model\":\"gpt-realtime-2.1-mini\"}}");
    CHECK(s_sent_n == 0);
    server_says("{\"type\":\"session.updated\",\"session\":{}}");
    CHECK(s_sent_n == 3);
    cJSON *info = sent_json(0), *greet = sent_json(1), *create = sent_json(2);
    CHECK(strcmp(jstr(info, "item.role"), "system") == 0);
    CHECK(strstr(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(
              cJSON_GetObjectItem(cJSON_GetObjectItem(info, "item"), "content"), 0), "text")), "01012345678") != NULL);
    CHECK(strcmp(jstr(greet, "item.role"), "user") == 0);
    CHECK(strcmp(jstr(create, "type"), "response.create") == 0);
    cJSON_Delete(info), cJSON_Delete(greet), cJSON_Delete(create);
    clear_sent();

    /* 상대 음성: 8kHz PCM → μ-law 로 대기열에 (통화 음성이 연결된 뒤에만) */
    uint8_t pcm[240];
    for (int i = 0; i < 120; i++) {
        int16_t v = (int16_t)(i * 250 - 15000);
        pcm[2 * i] = (uint8_t)(v & 0xff), pcm[2 * i + 1] = (uint8_t)((uint16_t)v >> 8);
    }
    realtime_push_caller_audio(pcm, sizeof(pcm));
    CHECK(s_caller_q_len == 0);
    realtime_audio_start(8000);
    realtime_push_caller_audio(pcm, sizeof(pcm));
    CHECK(s_caller_q_len == 120);
    CHECK(s_caller_q[0] == g711_ulaw_encode(-15000) && s_caller_q[119] == g711_ulaw_encode(119 * 250 - 15000));

    /* AI 음성 0.5초 (μ-law 4000 바이트, 여러 프레임 조각) → 0.3초가 모이면 폰으로 나간다 */
    server_says("{\"type\":\"response.created\",\"response\":{\"id\":\"resp_1\"}}");
    static uint8_t ulaw[4000];
    for (size_t i = 0; i < sizeof(ulaw); i++) {
        ulaw[i] = (uint8_t)(i * 13 + 5);
    }
    char *delta = audio_delta("item_42", ulaw, sizeof(ulaw));
    server_says(delta);
    free(delta);
    CHECK(realtime_pending_ai_audio() == 4000);
    uint8_t out[240];
    CHECK(realtime_pull_ai_audio(out, sizeof(out)) == sizeof(out));
    int16_t first = (int16_t)(out[0] | (out[1] << 8));
    CHECK(first == g711_ulaw_decode(ulaw[0]));
    for (int i = 0; i < 5; i++) {
        realtime_pull_ai_audio(out, sizeof(out));
    }
    /* 지금까지 120 × 6 = 720 바이트(90ms) 나갔다 */

    /* 상대가 끼어든다 → 남은 음성을 버리고, 들은 만큼(90ms)만 대화에 남긴다 */
    server_says("{\"type\":\"input_audio_buffer.speech_started\",\"audio_start_ms\":1000,\"item_id\":\"item_43\"}");
    CHECK(realtime_pending_ai_audio() == 0);
    CHECK(s_sent_n == 1);
    cJSON *tr = sent_json(0);
    CHECK(strcmp(jstr(tr, "type"), "conversation.item.truncate") == 0);
    CHECK(strcmp(jstr(tr, "item_id"), "item_42") == 0);
    CHECK(cJSON_GetNumberValue(cJSON_GetObjectItem(tr, "audio_end_ms")) == 90);
    cJSON_Delete(tr);
    clear_sent();

    /* 취소된 대답의 남은 조각은 버린다. 새 대답이 시작되면 다시 받는다. */
    delta = audio_delta("item_42", ulaw, 800);
    server_says(delta);
    free(delta);
    CHECK(realtime_pending_ai_audio() == 0);
    server_says("{\"type\":\"response.created\",\"response\":{\"id\":\"resp_2\"}}");
    delta = audio_delta("item_44", ulaw, 800);
    server_says(delta);
    free(delta);
    CHECK(realtime_pending_ai_audio() == 800);

    /* 대답이 끝나면 문턱(0.3초) 아래 꼬리도 바로 나간다 */
    server_says("{\"type\":\"response.output_audio.done\",\"item_id\":\"item_44\"}");
    CHECK(realtime_pull_ai_audio(out, sizeof(out)) == sizeof(out));

    /* 상대가 조용할 때 말을 시작하면 끼어들기가 아니다 */
    while (realtime_pull_ai_audio(out, sizeof(out)) > 0) {
    }
    server_says("{\"type\":\"response.done\",\"response\":{\"status\":\"completed\",\"output\":[]}}");
    server_says("{\"type\":\"input_audio_buffer.speech_started\",\"audio_start_ms\":5000,\"item_id\":\"item_45\"}");
    CHECK(s_sent_n == 0);

    /* 도구: 접수 메모 → 결과를 돌려주고 이어서 말하게 한다. 토큰 사용량도 읽는다. */
    server_says("{\"type\":\"response.done\",\"response\":{\"status\":\"completed\",\"output\":[{\"type\":\"function_call\","
                "\"name\":\"save_note\",\"call_id\":\"call_7\",\"arguments\":\"{\\\"request\\\":\\\"견적 요청\\\"}\"}],"
                "\"usage\":{\"input_tokens\":300,\"output_tokens\":120,\"input_token_details\":{\"text_tokens\":200,"
                "\"audio_tokens\":100,\"cached_tokens\":150},\"output_token_details\":{\"text_tokens\":20,\"audio_tokens\":100}}}}");
    CHECK(s_sent_n == 2);
    cJSON *fo = sent_json(0), *rc = sent_json(1);
    CHECK(strcmp(jstr(fo, "item.type"), "function_call_output") == 0);
    CHECK(strcmp(jstr(fo, "item.call_id"), "call_7") == 0);
    CHECK(strcmp(jstr(rc, "type"), "response.create") == 0);
    cJSON_Delete(fo), cJSON_Delete(rc);
    clear_sent();

    /* 도구: 사람에게 인계, 통화 끝내기 */
    server_says("{\"type\":\"response.done\",\"response\":{\"output\":[{\"type\":\"message\"},{\"type\":\"function_call\","
                "\"name\":\"transfer_to_human\",\"call_id\":\"call_8\",\"arguments\":\"{\\\"reason\\\":\\\"사람 요청\\\"}\"}]}}");
    CHECK(s_transfers == 1 && strcmp(s_transfer_reason, "사람 요청") == 0);
    server_says("{\"type\":\"response.done\",\"response\":{\"output\":[{\"type\":\"function_call\",\"name\":\"end_call\","
                "\"call_id\":\"call_9\",\"arguments\":\"{}\"}]}}");
    CHECK(s_end_calls == 1);

    /* 자막, 서버 오류는 로그만 */
    server_says("{\"type\":\"response.output_audio_transcript.done\",\"item_id\":\"item_44\",\"transcript\":\"안녕하세요\"}");
    server_says("{\"type\":\"error\",\"error\":{\"type\":\"invalid_request_error\",\"code\":\"invalid_value\",\"message\":\"bad\"}}");
    CHECK(s_sent_n == 0 && s_fails == 0);

    realtime_stop();
}

static void test_speed_test_and_errors(void)
{
    clear_sent();
    CHECK(realtime_start("", RT_OPEN_SPEED_TEST) == ESP_OK);
    esp_websocket_event_data_t conn = {.client = &s_client};
    fire(WEBSOCKET_EVENT_CONNECTED, &conn);
    server_says("{\"type\":\"session.updated\",\"session\":{}}");
    CHECK(s_sent_n == 3);  /* session.update, 시험 신호, 대답 요청 — 발신 번호는 보내지 않는다 */
    CHECK(strstr(s_sent[1], "속도 시험") != NULL);
    CHECK(strstr(s_sent[2], "response.create") != NULL);

    /* 연결 거절 (API 키 오류) → 실패 콜백 */
    esp_websocket_event_data_t err = {.client = &s_client, .error_handle = {.esp_ws_handshake_status_code = 401}};
    fire(WEBSOCKET_EVENT_ERROR, &err);
    CHECK(s_fails == 1);
    realtime_stop();
    clear_sent();
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    test_call();
    test_speed_test_and_errors();
    if (s_failures) {
        printf("%d FAILED\n", s_failures);
        return 1;
    }
    printf("realtime protocol tests passed\n");
    return 0;
}
