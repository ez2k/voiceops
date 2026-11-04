#include "gemini.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "b64.h"
#include "decimator.h"
#include "gemini_messages.h"
#include "gemini_stream.h"
#include "pcm_ring.h"

#define TAG "gemini"

#define WS_URI_PREFIX \
    "wss://generativelanguage.googleapis.com/ws/" \
    "google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key="

#define SEND_CHUNK_BYTES   640     /* 8kHz 16bit 40ms */
#define CALLER_BUF_BYTES   4096    /* 상대 음성 대기열 (약 0.25초) */
#define AI_RING_BYTES      16000   /* AI 음성 대기열 (1초). 차면 서버 수신을 잠시 멈춘다 */
#define PREBUFFER_BYTES    4800    /* 지터 버퍼: AI 음성을 0.3초 모은 뒤 내보내기 시작 (Wi-Fi 출렁임 흡수) */
#define REST_BUF_BYTES     6144    /* 음성을 뺀 서버 메시지 JSON */
#define TRANSCRIPT_MAX     400
#define WS_SEND_TIMEOUT    pdMS_TO_TICKS(2000)

/* Google Trust Services 루트 (google_roots.pem, 펌웨어에 내장) */
extern const char google_roots_pem[] asm("_binary_google_roots_pem_start");

static gemini_callbacks_t s_cb;
static gemini_config_t s_cfg;
static char s_uri[256];
static SemaphoreHandle_t s_ws_lock;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ready;      /* setupComplete 를 받았다 */
static volatile bool s_audio_on;   /* 통화 음성이 연결됐다 */
static volatile bool s_stopping;
static bool s_failed;
static char s_caller[32];

static StreamBufferHandle_t s_caller_audio;
static pcm_ring_t s_ai_audio;
static char *s_send_buf;
static size_t s_send_cap;

static gemini_stream_t s_stream;
static decimator_t s_decim;
static uint8_t s_carry;
static bool s_has_carry;
static char s_said_caller[TRANSCRIPT_MAX];
static char s_said_ai[TRANSCRIPT_MAX];

static volatile bool s_playing;    /* 지터 버퍼가 차서 AI 음성을 내보내는 중 */
static volatile bool s_turn_done;  /* AI 의 말 한 턴이 끝났다 → 남은 음성은 문턱 아래여도 내보낸다 */
static volatile uint32_t s_rx_bytes, s_underruns, s_send_fails;

static void fail_once(const char *why)
{
    s_ready = false;
    if (!s_stopping && !s_failed) {
        s_failed = true;
        s_cb.on_failed(why);
    }
}

/* ---- 보내기 ---- */

static void send_on(esp_websocket_client_handle_t client, const char *json, size_t len)
{
    if (esp_websocket_client_send_text(client, json, (int)len, WS_SEND_TIMEOUT) < 0) {
        s_send_fails++;
    }
}

/* 송신 태스크용: 세션이 닫히는 중에도 안전하게 */
static void ws_send(const char *json, size_t len)
{
    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    if (s_ws != NULL && esp_websocket_client_is_connected(s_ws)) {
        send_on(s_ws, json, len);
    }
    xSemaphoreGive(s_ws_lock);
}

/* WebSocket 이벤트 처리 중에는 그 태스크의 client 로 바로 보낸다 (s_ws_lock 을 잡으면 송신 태스크와 서로 기다린다) */
static void send_json(esp_websocket_client_handle_t client, cJSON *root)
{
    char *text = cJSON_PrintUnformatted(root);
    if (text != NULL) {
        send_on(client, text, strlen(text));
        cJSON_free(text);
    }
}

static bool add_text_part(cJSON *parts, cJSON *text)
{
    cJSON *part = cJSON_CreateObject();
    if (part == NULL || text == NULL || !cJSON_AddItemToObject(part, "text", text)) {
        cJSON_Delete(part);
        cJSON_Delete(text);
        return false;
    }
    return cJSON_AddItemToArray(parts, part);
}

/* 지시문은 복사하지 않고 가리키기만 한다 (PSRAM 이 없어 8KB 지시문 복사본도 부담). 이번 통화 정보는 별도 part. */
static bool send_setup(esp_websocket_client_handle_t client)
{
    char model[80], call_info[96];
    snprintf(model, sizeof(model), "models/%s", s_cfg.model);
    snprintf(call_info, sizeof(call_info), "## 이번 통화 정보\n- 상대 전화번호: %s\n",
             s_caller[0] ? s_caller : "알 수 없음");

    cJSON *root = cJSON_CreateObject();
    cJSON *setup = cJSON_AddObjectToObject(root, "setup");
    cJSON *gen = cJSON_AddObjectToObject(setup, "generationConfig");
    cJSON *modalities = cJSON_AddArrayToObject(gen, "responseModalities");
    cJSON *parts = cJSON_AddArrayToObject(cJSON_AddObjectToObject(setup, "systemInstruction"), "parts");
    cJSON *tools = cJSON_Parse(GEMINI_TOOLS_JSON);
    bool ok = parts != NULL && modalities != NULL &&
              cJSON_AddStringToObject(setup, "model", model) != NULL &&
              cJSON_AddItemToArray(modalities, cJSON_CreateString("AUDIO")) &&
              add_text_part(parts, cJSON_CreateStringReference(s_cfg.prompt)) &&
              add_text_part(parts, cJSON_CreateString(call_info)) &&
              tools != NULL && cJSON_AddItemToObject(setup, "tools", tools) &&
              cJSON_AddObjectToObject(setup, "inputAudioTranscription") != NULL &&
              cJSON_AddObjectToObject(setup, "outputAudioTranscription") != NULL;
    if (!ok && cJSON_GetObjectItem(setup, "tools") != tools) {
        cJSON_Delete(tools);  /* 트리에 붙지 못했으면 따로 지운다 */
    }
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) {
        return false;
    }
    send_on(client, text, strlen(text));
    cJSON_free(text);
    return true;
}

/* 상대 음성을 40ms 씩 모아 보내는 태스크 */
static void sender_task(void *arg)
{
    static uint8_t pcm[SEND_CHUNK_BYTES];
    const size_t prefix = strlen(GEMINI_AUDIO_PREFIX);
    const size_t suffix = strlen(GEMINI_AUDIO_SUFFIX);

    for (;;) {
        size_t n = xStreamBufferReceive(s_caller_audio, pcm, sizeof(pcm), portMAX_DELAY);
        if (n == 0 || !s_ready || !s_audio_on) {
            continue;  /* 세션이 준비되기 전 소리는 버린다 */
        }
        memcpy(s_send_buf, GEMINI_AUDIO_PREFIX, prefix);
        size_t len = prefix + b64_encode(pcm, n, s_send_buf + prefix);
        memcpy(s_send_buf + len, GEMINI_AUDIO_SUFFIX, suffix);
        ws_send(s_send_buf, len + suffix);
    }
}

/* ---- 받기: AI 음성 ---- */

static void ring_write_waiting(const uint8_t *data, size_t len)
{
    /* AI 음성은 실시간보다 빨리 온다. 버퍼가 차면 여기서 기다리고, 그동안 서버 수신도 멈춘다. */
    while (len > 0 && !s_stopping) {
        size_t n = pcm_ring_write(&s_ai_audio, data, len);
        data += n;
        len -= n;
        if (len > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

static void on_ai_audio_bytes(const uint8_t *bytes, size_t len, void *ctx)
{
    s_turn_done = false;  /* 새 음성이 오는 중 */
    int16_t out[48];
    size_t n_out = 0;

    for (size_t i = 0; i < len; i++) {
        if (!s_has_carry) {
            s_carry = bytes[i];
            s_has_carry = true;
            continue;
        }
        s_has_carry = false;
        int16_t sample = (int16_t)(s_carry | ((uint16_t)bytes[i] << 8));  /* 16bit little-endian */
        if (decimator_push(&s_decim, sample, &out[n_out])) {
            if (++n_out == sizeof(out) / sizeof(out[0])) {
                ring_write_waiting((const uint8_t *)out, sizeof(out));
                n_out = 0;
            }
        }
    }
    if (n_out > 0) {
        ring_write_waiting((const uint8_t *)out, n_out * sizeof(out[0]));
    }
}

/* ---- 받기: 제어 메시지 ---- */

static void say_append(char *buf, const cJSON *transcription)
{
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(transcription, "text"));
    if (text != NULL) {
        strlcat(buf, text, TRANSCRIPT_MAX);
    }
}

static void say_flush(void)
{
    if (s_said_caller[0]) {
        ESP_LOGI(TAG, "상대: %s", s_said_caller);
        s_said_caller[0] = '\0';
    }
    if (s_said_ai[0]) {
        ESP_LOGI(TAG, "AI: %s", s_said_ai);
        s_said_ai[0] = '\0';
    }
}

static void handle_tool_calls(esp_websocket_client_handle_t client, const cJSON *tool_call)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *responses = cJSON_AddArrayToObject(cJSON_AddObjectToObject(root, "toolResponse"), "functionResponses");
    const cJSON *fc;

    cJSON_ArrayForEach(fc, cJSON_GetObjectItemCaseSensitive(tool_call, "functionCalls")) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fc, "id"));
        const char *name = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(fc, "name"));
        const cJSON *args = cJSON_GetObjectItemCaseSensitive(fc, "args");
        char *args_text = args ? cJSON_PrintUnformatted(args) : NULL;
        ESP_LOGI(TAG, "AI 도구: %s %s", name ? name : "?", args_text ? args_text : "{}");

        cJSON *result = cJSON_CreateObject();
        const char *scheduling = "SILENT";
        if (name != NULL && strcmp(name, "end_call") == 0) {
            s_cb.on_end_call();
            cJSON_AddBoolToObject(result, "ok", true);
        } else if (name != NULL && strcmp(name, "transfer_to_human") == 0) {
            const char *reason = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(args, "reason"));
            s_cb.on_transfer(reason ? reason : "");
            cJSON_AddBoolToObject(result, "ok", true);
        } else if (name != NULL && strcmp(name, "save_note") == 0) {
            /* 1차 실험: 접수 메모는 시리얼 로그로만 남긴다 */
            ESP_LOGW(TAG, "접수 메모 (발신 %s): %s", s_caller[0] ? s_caller : "알 수 없음", args_text ? args_text : "{}");
            cJSON_AddBoolToObject(result, "ok", true);
            cJSON_AddStringToObject(result, "message", "접수 메모를 저장했습니다");
            scheduling = "WHEN_IDLE";
        } else {
            cJSON_AddStringToObject(result, "error", "알 수 없는 도구");
            scheduling = "WHEN_IDLE";
        }

        cJSON *resp = cJSON_CreateObject();
        if (id != NULL) {
            cJSON_AddStringToObject(resp, "id", id);
        }
        cJSON_AddStringToObject(resp, "name", name ? name : "");
        cJSON_AddItemToObject(resp, "response", result);
        cJSON_AddStringToObject(resp, "scheduling", scheduling);
        cJSON_AddItemToArray(responses, resp);
        cJSON_free(args_text);
    }

    send_json(client, root);
    cJSON_Delete(root);
}

static void handle_message(esp_websocket_client_handle_t client, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    if (root == NULL) {
        ESP_LOGW(TAG, "unparsable message (%u bytes)", (unsigned)strlen(json));
        return;
    }

    if (cJSON_GetObjectItemCaseSensitive(root, "setupComplete") != NULL) {
        ESP_LOGI(TAG, "session ready");
        s_ready = true;
        send_on(client, GEMINI_GREETING_JSON, strlen(GEMINI_GREETING_JSON));
    }

    const cJSON *sc = cJSON_GetObjectItemCaseSensitive(root, "serverContent");
    if (sc != NULL) {
        bool interrupted = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "interrupted"));
        if (interrupted) {
            /* 상대가 말을 끊었다: 아직 안 나간 AI 음성을 버린다 */
            pcm_ring_clear(&s_ai_audio);
            decimator_reset(&s_decim);
            s_has_carry = false;
            s_playing = false;
        }
        if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "generationComplete")) ||
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "turnComplete"))) {
            s_turn_done = true;
        }
        say_append(s_said_caller, cJSON_GetObjectItemCaseSensitive(sc, "inputTranscription"));
        say_append(s_said_ai, cJSON_GetObjectItemCaseSensitive(sc, "outputTranscription"));
        if (interrupted || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(sc, "turnComplete"))) {
            say_flush();
        }
    }

    const cJSON *tool_call = cJSON_GetObjectItemCaseSensitive(root, "toolCall");
    if (tool_call != NULL) {
        handle_tool_calls(client, tool_call);
    }

    const cJSON *go_away = cJSON_GetObjectItemCaseSensitive(root, "goAway");
    if (go_away != NULL) {
        const char *left = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(go_away, "timeLeft"));
        ESP_LOGW(TAG, "session ending soon (%s left)", left ? left : "?");
    }

    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const esp_websocket_event_data_t *d = event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected, sending setup");
        if (!send_setup(d->client)) {
            ESP_LOGE(TAG, "setup 메시지를 만들지 못했습니다 (메모리 부족)");
            fail_once("setup");
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        s_rx_bytes += (uint32_t)d->data_len;
        if (d->op_code == WS_TRANSPORT_OPCODES_CLOSE) {
            /* 서버가 닫으며 이유를 알려 준다 (예: API 키 오류). 앞 2바이트는 상태 코드. */
            int text_len = d->data_len > 2 ? d->data_len - 2 : 0;
            ESP_LOGW(TAG, "server closed: %.*s", text_len, text_len ? d->data_ptr + 2 : "");
            break;
        }
        if (d->op_code != WS_TRANSPORT_OPCODES_TEXT && d->op_code != WS_TRANSPORT_OPCODES_BINARY &&
            d->op_code != WS_TRANSPORT_OPCODES_CONT) {
            break;  /* ping/pong */
        }
        if (d->payload_offset == 0 && d->op_code != WS_TRANSPORT_OPCODES_CONT) {
            gs_begin(&s_stream);
        }
        gs_feed(&s_stream, d->data_ptr, (size_t)d->data_len);
        if (d->fin && d->payload_offset + d->data_len >= d->payload_len) {
            const char *json = gs_end(&s_stream);
            if (json != NULL) {
                handle_message(d->client, json);
            } else {
                ESP_LOGW(TAG, "control part of message too large, skipped");
            }
        }
        break;

    case WEBSOCKET_EVENT_DISCONNECTED:
        fail_once("disconnected");
        break;
    case WEBSOCKET_EVENT_CLOSED:
        fail_once("closed by server");
        break;
    case WEBSOCKET_EVENT_ERROR:
        fail_once("connection error");
        break;
    default:
        break;
    }
}

/* ---- 공개 함수 ---- */

esp_err_t gemini_init(const gemini_callbacks_t *callbacks, const gemini_config_t *config)
{
    s_cb = *callbacks;
    s_cfg = *config;
    snprintf(s_uri, sizeof(s_uri), "%s%s", WS_URI_PREFIX, s_cfg.api_key);
    decimator_init();
    s_ws_lock = xSemaphoreCreateMutex();
    s_caller_audio = xStreamBufferCreate(CALLER_BUF_BYTES, SEND_CHUNK_BYTES);
    s_send_cap = strlen(GEMINI_AUDIO_PREFIX) + B64_ENCODED_LEN(SEND_CHUNK_BYTES) + strlen(GEMINI_AUDIO_SUFFIX);
    s_send_buf = malloc(s_send_cap);
    char *rest = malloc(REST_BUF_BYTES);
    if (s_ws_lock == NULL || s_caller_audio == NULL || s_send_buf == NULL || rest == NULL ||
        !pcm_ring_init(&s_ai_audio, AI_RING_BYTES)) {
        return ESP_ERR_NO_MEM;
    }
    gs_init(&s_stream, rest, REST_BUF_BYTES, on_ai_audio_bytes, NULL);
    if (xTaskCreate(sender_task, "gemini_tx", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t gemini_start(const char *caller_number)
{
    gemini_stop();

    strlcpy(s_caller, caller_number ? caller_number : "", sizeof(s_caller));
    s_stopping = false;
    s_failed = false;
    s_ready = false;
    s_audio_on = false;
    pcm_ring_clear(&s_ai_audio);
    decimator_reset(&s_decim);
    s_has_carry = false;
    s_said_caller[0] = '\0';
    s_said_ai[0] = '\0';
    s_playing = false;
    s_turn_done = false;
    s_rx_bytes = 0;
    s_underruns = 0;
    s_send_fails = 0;

    const esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .buffer_size = 2048,
        .task_stack = 6144,
        .task_prio = 5,
        .cert_pem = google_roots_pem,
        .disable_auto_reconnect = true,
        .network_timeout_ms = 10000,
        .keep_alive_enable = true,  /* 죽은 연결을 TCP keepalive 로 약 20초 안에 끊는다 (기본 5s/5s/3회) */
    };
    esp_websocket_client_handle_t ws = esp_websocket_client_init(&cfg);
    if (ws == NULL) {
        return ESP_ERR_NO_MEM;
    }
    esp_websocket_register_events(ws, WEBSOCKET_EVENT_ANY, ws_event, NULL);

    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    s_ws = ws;
    xSemaphoreGive(s_ws_lock);

    esp_err_t err = esp_websocket_client_start(ws);
    if (err != ESP_OK) {
        gemini_stop();
    }
    return err;
}

void gemini_audio_start(int rate)
{
    if (rate != GEMINI_CALL_RATE) {
        ESP_LOGE(TAG, "unsupported call sample rate %d", rate);
        return;
    }
    s_audio_on = true;
}

void gemini_push_caller_audio(const uint8_t *pcm, size_t len)
{
    if (s_ready && s_audio_on) {
        xStreamBufferSend(s_caller_audio, pcm, len, 0);
    }
}

size_t gemini_pull_ai_audio(uint8_t *pcm, size_t len)
{
    /* 지터 버퍼: 0.3초가 모이기 전에는 무음. 말 끝(턴 종료)의 짧은 꼬리는 바로 내보낸다. */
    if (!s_playing) {
        size_t have = pcm_ring_count(&s_ai_audio);
        if (have < PREBUFFER_BYTES && !(s_turn_done && have > 0)) {
            return 0;
        }
        s_playing = true;
    }
    size_t n = pcm_ring_read(&s_ai_audio, pcm, len);
    if (n < len) {
        s_playing = false;  /* 바닥났다 → 다시 모은다 */
        if (!s_turn_done) {
            s_underruns++;  /* 말하는 도중에 음성이 늦게 왔다 (네트워크가 못 따라옴) */
        }
    }
    return n;
}

void gemini_get_stats(gemini_stats_t *st)
{
    st->rx_bytes = s_rx_bytes;
    st->underruns = s_underruns;
    st->send_fails = s_send_fails;
    st->ai_buffered_ms = (uint32_t)(pcm_ring_count(&s_ai_audio) * 1000 / (GEMINI_CALL_RATE * 2));
}

size_t gemini_pending_ai_audio(void)
{
    return pcm_ring_count(&s_ai_audio);
}

void gemini_stop(void)
{
    s_stopping = true;
    s_ready = false;
    s_audio_on = false;

    xSemaphoreTake(s_ws_lock, portMAX_DELAY);
    esp_websocket_client_handle_t ws = s_ws;
    s_ws = NULL;
    xSemaphoreGive(s_ws_lock);

    if (ws != NULL) {
        if (esp_websocket_client_is_connected(ws)) {
            esp_websocket_client_close(ws, pdMS_TO_TICKS(1000));
        }
        esp_websocket_client_destroy(ws);
        say_flush();
        ESP_LOGI(TAG, "session closed");
    }
    pcm_ring_clear(&s_ai_audio);
}
