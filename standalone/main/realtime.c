#include "realtime.h"

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
#include "g711.h"
#include "pcm_ring.h"
#include "realtime_messages.h"
#include "rt_stream.h"

#define TAG "realtime"

#define WS_URI_PREFIX      "wss://api.openai.com/v1/realtime?model="
#define ULAW_PER_MS        8       /* μ-law 8kHz: 1ms = 8바이트 */
#define SEND_CHUNK_BYTES   800     /* 상대 음성 100ms 씩 보낸다 (블루투스와 무선을 나눠 쓰니 횟수를 줄인다) */
#define CALLER_BUF_BYTES   2400    /* 상대 음성 대기열 0.3초 */
#define AI_RING_BYTES      16000   /* AI 음성 대기열 2초. 차면 서버 수신을 잠시 멈춘다 */
#define PREBUFFER_MIN      2400    /* 지터 버퍼: AI 음성을 0.3초 모은 뒤 내보내기 시작 (Wi-Fi 출렁임 흡수) */
#define PREBUFFER_STEP     1600    /* 말하는 도중 바닥나면 0.2초씩 더 모은다 (적응형) */
#define PREBUFFER_MAX      9600    /* 최대 1.2초 */
#define REST_BUF_BYTES     6144    /* 음성을 뺀 서버 메시지 JSON */
#define ITEM_ID_MAX        48
#define WS_SEND_TIMEOUT    pdMS_TO_TICKS(5000)  /* 통화 중 패킷을 잃으면 TCP 재전송에 1~2초가 걸린다 — 그 사이 끊지 않게 */

/* Google Trust Services 루트 (google_roots.pem) — api.openai.com 인증서도 GTS(WE1 → GTS Root R4)가 발급했다 */
extern const char google_roots_pem[] asm("_binary_google_roots_pem_start");

static realtime_callbacks_t s_cb;
static realtime_config_t s_cfg;
static char s_uri[160];
static char s_headers[320];
static SemaphoreHandle_t s_ws_lock;
static esp_websocket_client_handle_t s_ws;
static volatile bool s_ready;      /* session.updated 를 받았다 */
static volatile bool s_audio_on;   /* 통화 음성이 연결됐다 */
static volatile bool s_stopping;
static bool s_failed;
static char s_caller[32];
static rt_opening_t s_opening;

static StreamBufferHandle_t s_caller_audio;   /* μ-law */
static pcm_ring_t s_ai_audio;                 /* μ-law */
static char *s_send_buf;

static rt_stream_t s_stream;
static volatile bool s_playing;     /* 지터 버퍼가 차서 AI 음성을 내보내는 중 */
static volatile bool s_turn_done;   /* AI 의 대답 음성이 끝났다 → 남은 음성은 문턱 아래여도 내보낸다 */
static volatile bool s_discarding;  /* 상대가 끼어들어 취소된 대답의 남은 음성을 버리는 중 */
static volatile uint32_t s_rx_bytes, s_underruns, s_send_fails;
static volatile size_t s_prebuffer = PREBUFFER_MIN;

/* 끼어들기: 지금 나가는 AI 음성 항목과 그 항목에서 받은 바이트 → 상대가 들은 만큼만 대화에 남긴다 */
static char s_audio_item[ITEM_ID_MAX];
static uint32_t s_item_bytes;
static uint32_t s_msg_bytes;        /* 지금 받는 메시지에서 풀린 음성 바이트 */

/* 이번 세션 토큰 사용량 (요금 확인용) */
static struct {
    uint32_t in_audio, in_text, in_cached, out_audio, out_text;
} s_usage;

static void fail_once(const char *why)
{
    s_ready = false;
    if (!s_stopping && !s_failed) {
        s_failed = true;
        s_cb.on_failed(why);
    }
}

/* ---- JSON 도우미 ---- */

static const char *str_of(const cJSON *obj, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static uint32_t num_of(const cJSON *obj, const char *key)
{
    const cJSON *n = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsNumber(n) && n->valuedouble > 0 ? (uint32_t)n->valuedouble : 0;
}

/* item 을 obj 에 붙인다. 못 붙이면 item 을 지운다 (만들다 실패해도 새지 않게). */
static bool attach(cJSON *obj, const char *key, cJSON *item)
{
    if (obj == NULL || item == NULL || !cJSON_AddItemToObject(obj, key, item)) {
        cJSON_Delete(item);
        return false;
    }
    return true;
}

static cJSON *type_obj(const char *type)
{
    cJSON *o = cJSON_CreateObject();
    if (o != NULL && cJSON_AddStringToObject(o, "type", type) == NULL) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

/* ---- 보내기 ---- */

static void send_on(esp_websocket_client_handle_t client, const char *json, size_t len)
{
    if (esp_websocket_client_send_text(client, json, (int)len, WS_SEND_TIMEOUT) < 0) {
        s_send_fails++;
    }
}

static void send_str(esp_websocket_client_handle_t client, const char *json)
{
    send_on(client, json, strlen(json));
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
static bool send_json(esp_websocket_client_handle_t client, cJSON *root, bool ok)
{
    char *text = ok ? cJSON_PrintUnformatted(root) : NULL;
    cJSON_Delete(root);
    if (text == NULL) {
        return false;
    }
    send_str(client, text);
    cJSON_free(text);
    return true;
}

/* 세션 설정: 지시문은 복사하지 않고 가리키기만 한다 (PSRAM 이 없어 8KB 지시문 복사본도 부담) */
static bool send_session_update(esp_websocket_client_handle_t client)
{
    cJSON *root = type_obj("session.update");
    cJSON *session = type_obj("realtime");
    if (!attach(root, "session", session)) {
        cJSON_Delete(root);
        return false;
    }
    /* 아래는 모두 트리 안에서 만든다 — 중간에 실패해도 root 하나만 지우면 된다 */
    cJSON *audio = cJSON_AddObjectToObject(session, "audio");
    cJSON *in = cJSON_AddObjectToObject(audio, "input");
    cJSON *out = cJSON_AddObjectToObject(audio, "output");
    cJSON *modalities = cJSON_AddArrayToObject(session, "output_modalities");
    bool ok = in != NULL && out != NULL && modalities != NULL &&
              cJSON_AddItemToArray(modalities, cJSON_CreateString("audio")) &&
              attach(session, "instructions", cJSON_CreateStringReference(s_cfg.prompt)) &&
              attach(in, "format", type_obj(RT_AUDIO_TYPE)) &&
              attach(in, "turn_detection", type_obj("server_vad")) &&
              attach(out, "format", type_obj(RT_AUDIO_TYPE)) &&
              cJSON_AddStringToObject(out, "voice", RT_VOICE) != NULL &&
              attach(session, "tools", cJSON_Parse(RT_TOOLS_JSON)) &&
              cJSON_AddStringToObject(session, "tool_choice", "auto") != NULL;
    return send_json(client, root, ok);
}

/* 발신 번호는 시스템 메시지로 따로 넣는다 (지시문을 복사해 붙이지 않으려고) */
static bool send_call_info(esp_websocket_client_handle_t client)
{
    char text[96];
    snprintf(text, sizeof(text), "이번 통화 정보: 상대 전화번호 %s", s_caller[0] ? s_caller : "알 수 없음");
    cJSON *root = type_obj("conversation.item.create");
    cJSON *item = type_obj("message");
    if (!attach(root, "item", item)) {
        cJSON_Delete(root);
        return false;
    }
    cJSON *content = cJSON_AddArrayToObject(item, "content");
    cJSON *part = type_obj("input_text");
    if (part != NULL && !cJSON_AddItemToArray(content, part)) {
        cJSON_Delete(part);
        part = NULL;
    }
    bool ok = part != NULL && cJSON_AddStringToObject(item, "role", "system") != NULL &&
              cJSON_AddStringToObject(part, "text", text) != NULL;
    return send_json(client, root, ok);
}

static void send_opening(esp_websocket_client_handle_t client)
{
    if (s_opening == RT_OPEN_SPEED_TEST) {
        send_str(client, RT_SPEED_TEST_JSON);
    } else {
        send_call_info(client);
        send_str(client, RT_GREETING_JSON);
    }
    send_str(client, RT_RESPONSE_CREATE_JSON);
}

static void send_function_output(esp_websocket_client_handle_t client, const char *call_id, const char *output)
{
    cJSON *root = type_obj("conversation.item.create");
    cJSON *item = type_obj("function_call_output");
    bool ok = attach(root, "item", item) && cJSON_AddStringToObject(item, "call_id", call_id ? call_id : "") &&
              cJSON_AddStringToObject(item, "output", output);
    send_json(client, root, ok);
    send_str(client, RT_RESPONSE_CREATE_JSON);  /* 결과를 받고 이어서 말하게 한다 */
}

/* 상대 음성(μ-law)을 100ms 씩 모아 보내는 태스크 */
static void sender_task(void *arg)
{
    static uint8_t ulaw[SEND_CHUNK_BYTES];
    const size_t prefix = strlen(RT_AUDIO_PREFIX);
    const size_t suffix = strlen(RT_AUDIO_SUFFIX);

    for (;;) {
        size_t n = xStreamBufferReceive(s_caller_audio, ulaw, sizeof(ulaw), portMAX_DELAY);
        if (n == 0 || !s_ready || !s_audio_on) {
            continue;  /* 세션이 준비되기 전 소리는 버린다 */
        }
        memcpy(s_send_buf, RT_AUDIO_PREFIX, prefix);
        size_t len = prefix + b64_encode(ulaw, n, s_send_buf + prefix);
        memcpy(s_send_buf + len, RT_AUDIO_SUFFIX, suffix);
        ws_send(s_send_buf, len + suffix);
    }
}

/* ---- 받기: AI 음성 ---- */

static void on_ai_audio_bytes(const uint8_t *bytes, size_t len, void *ctx)
{
    if (s_discarding) {
        return;  /* 상대가 끼어들어 취소된 대답의 남은 조각 */
    }
    s_turn_done = false;
    s_msg_bytes += (uint32_t)len;
    /* AI 음성은 실시간보다 빨리 온다. 대기열이 차면 여기서 기다리고, 그동안 서버 수신도 멈춘다. */
    while (len > 0 && !s_stopping && !s_discarding) {
        size_t n = pcm_ring_write(&s_ai_audio, bytes, len);
        bytes += n;
        len -= n;
        if (len > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
}

/* ---- 받기: 제어 메시지 ---- */

/* 상대가 말을 시작했다: AI 가 말하는 중이었으면 멈추고, 상대가 들은 만큼만 대화에 남긴다 */
static void on_speech_started(esp_websocket_client_handle_t client)
{
    size_t queued = pcm_ring_count(&s_ai_audio);
    if (s_audio_item[0] == '\0' || (queued == 0 && s_turn_done)) {
        return;  /* AI 는 말하고 있지 않았다 */
    }
    s_discarding = true;
    pcm_ring_clear(&s_ai_audio);
    s_playing = false;
    uint32_t played_ms = (s_item_bytes > queued ? s_item_bytes - (uint32_t)queued : 0) / ULAW_PER_MS;
    char msg[ITEM_ID_MAX + 120];
    snprintf(msg, sizeof(msg),
             "{\"type\":\"conversation.item.truncate\",\"item_id\":\"%s\",\"content_index\":0,\"audio_end_ms\":%u}",
             s_audio_item, (unsigned)played_ms);
    send_str(client, msg);
    ESP_LOGI(TAG, "상대가 끼어들어 AI 말을 %u ms 에서 멈춥니다", (unsigned)played_ms);
    s_audio_item[0] = '\0';
}

static void handle_function_call(esp_websocket_client_handle_t client, const cJSON *item)
{
    const char *name = str_of(item, "name");
    const char *call_id = str_of(item, "call_id");
    const char *args_text = str_of(item, "arguments");
    ESP_LOGI(TAG, "AI 도구: %s %s", name ? name : "?", args_text ? args_text : "{}");

    if (name != NULL && strcmp(name, "end_call") == 0) {
        s_cb.on_end_call();
    } else if (name != NULL && strcmp(name, "transfer_to_human") == 0) {
        cJSON *args = args_text ? cJSON_Parse(args_text) : NULL;
        const char *reason = str_of(args, "reason");
        s_cb.on_transfer(reason ? reason : "");
        cJSON_Delete(args);
    } else if (name != NULL && strcmp(name, "save_note") == 0) {
        /* 1차 실험: 접수 메모는 시리얼 로그로만 남긴다 */
        ESP_LOGW(TAG, "접수 메모 (발신 %s): %s", s_caller[0] ? s_caller : "알 수 없음", args_text ? args_text : "{}");
        send_function_output(client, call_id, "{\"ok\":true,\"message\":\"접수 메모를 저장했습니다\"}");
    } else {
        send_function_output(client, call_id, "{\"error\":\"알 수 없는 도구\"}");
    }
}

static void handle_response_done(esp_websocket_client_handle_t client, const cJSON *root)
{
    s_turn_done = true;
    const cJSON *resp = cJSON_GetObjectItemCaseSensitive(root, "response");
    const char *status = str_of(resp, "status");
    if (status != NULL && strcmp(status, "failed") == 0) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(resp, "status_details"), "error");
        ESP_LOGE(TAG, "대답 실패: %s", str_of(err, "message") ? str_of(err, "message") : "?");
    }

    const cJSON *usage = cJSON_GetObjectItemCaseSensitive(resp, "usage");
    const cJSON *in = cJSON_GetObjectItemCaseSensitive(usage, "input_token_details");
    const cJSON *out = cJSON_GetObjectItemCaseSensitive(usage, "output_token_details");
    s_usage.in_audio += num_of(in, "audio_tokens");
    s_usage.in_text += num_of(in, "text_tokens");
    s_usage.in_cached += num_of(in, "cached_tokens");
    s_usage.out_audio += num_of(out, "audio_tokens");
    s_usage.out_text += num_of(out, "text_tokens");

    const cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(resp, "output")) {
        const char *type = str_of(item, "type");
        if (type != NULL && strcmp(type, "function_call") == 0) {
            handle_function_call(client, item);
        }
    }
}

static void handle_message(esp_websocket_client_handle_t client, const char *json)
{
    cJSON *root = cJSON_Parse(json);
    const char *type = str_of(root, "type");
    if (type == NULL) {
        ESP_LOGW(TAG, "unparsable message (%u bytes)", (unsigned)strlen(json));
        cJSON_Delete(root);
        return;
    }

    if (strcmp(type, "response.output_audio.delta") == 0) {
        const char *item = str_of(root, "item_id");
        if (item != NULL && strcmp(item, s_audio_item) != 0) {
            strlcpy(s_audio_item, item, sizeof(s_audio_item));
            s_item_bytes = 0;
        }
        s_item_bytes += s_msg_bytes;
    } else if (strcmp(type, "session.created") == 0) {
        const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
        ESP_LOGI(TAG, "connected (%s)", str_of(session, "model") ? str_of(session, "model") : "?");
    } else if (strcmp(type, "session.updated") == 0) {
        if (!s_ready) {
            s_ready = true;
            ESP_LOGI(TAG, "session ready");
            send_opening(client);
        }
    } else if (strcmp(type, "response.created") == 0) {
        s_discarding = false;
        s_turn_done = false;
    } else if (strcmp(type, "response.output_audio.done") == 0) {
        s_turn_done = true;
    } else if (strcmp(type, "response.output_audio_transcript.done") == 0) {
        ESP_LOGI(TAG, "AI: %s", str_of(root, "transcript") ? str_of(root, "transcript") : "");
    } else if (strcmp(type, "conversation.item.input_audio_transcription.completed") == 0) {
        ESP_LOGI(TAG, "상대: %s", str_of(root, "transcript") ? str_of(root, "transcript") : "");
    } else if (strcmp(type, "input_audio_buffer.speech_started") == 0) {
        on_speech_started(client);
    } else if (strcmp(type, "response.done") == 0) {
        handle_response_done(client, root);
    } else if (strcmp(type, "error") == 0) {
        const cJSON *err = cJSON_GetObjectItemCaseSensitive(root, "error");
        ESP_LOGE(TAG, "서버 오류 (%s): %s", str_of(err, "code") ? str_of(err, "code") : "?",
                 str_of(err, "message") ? str_of(err, "message") : "?");
    }
    cJSON_Delete(root);
}

static void ws_event(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    const esp_websocket_event_data_t *d = event_data;

    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "connected, sending session.update");
        if (!send_session_update(d->client)) {
            ESP_LOGE(TAG, "session.update 를 만들지 못했습니다 (메모리 부족)");
            fail_once("setup");
        }
        break;

    case WEBSOCKET_EVENT_DATA:
        s_rx_bytes += (uint32_t)d->data_len;
        if (d->op_code == WS_TRANSPORT_OPCODES_CLOSE) {
            /* 서버가 닫으며 이유를 알려 준다. 앞 2바이트는 상태 코드. */
            int text_len = d->data_len > 2 ? d->data_len - 2 : 0;
            ESP_LOGW(TAG, "server closed: %.*s", text_len, text_len ? d->data_ptr + 2 : "");
            break;
        }
        if (d->op_code != WS_TRANSPORT_OPCODES_TEXT && d->op_code != WS_TRANSPORT_OPCODES_BINARY &&
            d->op_code != WS_TRANSPORT_OPCODES_CONT) {
            break;  /* ping/pong */
        }
        if (d->payload_offset == 0 && d->op_code != WS_TRANSPORT_OPCODES_CONT) {
            rs_begin(&s_stream);
            s_msg_bytes = 0;
        }
        rs_feed(&s_stream, d->data_ptr, (size_t)d->data_len);
        if (d->fin && d->payload_offset + d->data_len >= d->payload_len) {
            const char *json = rs_end(&s_stream);
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
        if (d->error_handle.esp_ws_handshake_status_code != 0) {
            /* 401 = API 키 오류, 404 = 모델 이름 오류, 429 = 한도·잔액 부족 */
            ESP_LOGE(TAG, "서버가 연결을 거절했습니다 (HTTP %d) — API 키·모델 이름·잔액을 확인하세요",
                     d->error_handle.esp_ws_handshake_status_code);
        }
        fail_once("connection error");
        break;
    default:
        break;
    }
}

/* ---- 공개 함수 ---- */

esp_err_t realtime_init(const realtime_callbacks_t *callbacks, const realtime_config_t *config)
{
    s_cb = *callbacks;
    s_cfg = *config;
    snprintf(s_uri, sizeof(s_uri), "%s%s", WS_URI_PREFIX, s_cfg.model);
    snprintf(s_headers, sizeof(s_headers), "Authorization: Bearer %s\r\n", s_cfg.api_key);
    s_ws_lock = xSemaphoreCreateMutex();
    s_caller_audio = xStreamBufferCreate(CALLER_BUF_BYTES, SEND_CHUNK_BYTES);
    s_send_buf = malloc(strlen(RT_AUDIO_PREFIX) + B64_ENCODED_LEN(SEND_CHUNK_BYTES) + strlen(RT_AUDIO_SUFFIX));
    char *rest = malloc(REST_BUF_BYTES);
    if (s_ws_lock == NULL || s_caller_audio == NULL || s_send_buf == NULL || rest == NULL ||
        !pcm_ring_init(&s_ai_audio, AI_RING_BYTES)) {
        return ESP_ERR_NO_MEM;
    }
    rs_init(&s_stream, rest, REST_BUF_BYTES, "delta", "\"response.output_audio.delta\"", on_ai_audio_bytes, NULL);
    if (xTaskCreate(sender_task, "rt_tx", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t realtime_start(const char *caller_number, rt_opening_t opening)
{
    realtime_stop();

    s_opening = opening;
    strlcpy(s_caller, caller_number ? caller_number : "", sizeof(s_caller));
    s_stopping = false;
    s_failed = false;
    s_ready = false;
    s_audio_on = false;
    pcm_ring_clear(&s_ai_audio);
    s_playing = false;
    s_turn_done = false;
    s_discarding = false;
    s_audio_item[0] = '\0';
    s_item_bytes = 0;
    s_rx_bytes = 0;
    s_underruns = 0;
    s_send_fails = 0;
    s_prebuffer = PREBUFFER_MIN;
    memset(&s_usage, 0, sizeof(s_usage));

    const esp_websocket_client_config_t cfg = {
        .uri = s_uri,
        .headers = s_headers,
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
        realtime_stop();
    }
    return err;
}

void realtime_audio_start(int rate)
{
    if (rate != RT_CALL_RATE) {
        ESP_LOGE(TAG, "unsupported call sample rate %d", rate);
        return;
    }
    s_audio_on = true;
}

void realtime_push_caller_audio(const uint8_t *pcm, size_t len)
{
    if (!s_ready || !s_audio_on) {
        return;
    }
    uint8_t ulaw[120];
    size_t n = 0;
    for (size_t i = 0; i + 1 < len; i += 2) {
        ulaw[n++] = g711_ulaw_encode((int16_t)(pcm[i] | (pcm[i + 1] << 8)));  /* 16bit little-endian */
        if (n == sizeof(ulaw)) {
            xStreamBufferSend(s_caller_audio, ulaw, n, 0);
            n = 0;
        }
    }
    if (n > 0) {
        xStreamBufferSend(s_caller_audio, ulaw, n, 0);
    }
}

size_t realtime_pull_ai_audio(uint8_t *pcm, size_t len)
{
    const size_t want = len / 2;  /* PCM 2바이트 = μ-law 1바이트 */
    /* 지터 버퍼: 문턱만큼 모이기 전에는 무음. 대답의 끝부분은 문턱 아래여도 바로 내보낸다. */
    if (!s_playing) {
        size_t have = pcm_ring_count(&s_ai_audio);
        if (have < s_prebuffer && !(s_turn_done && have > 0)) {
            return 0;
        }
        s_playing = true;
    }
    uint8_t ulaw[120];
    size_t done = 0;
    while (done < want) {
        size_t k = want - done < sizeof(ulaw) ? want - done : sizeof(ulaw);
        size_t n = pcm_ring_read(&s_ai_audio, ulaw, k);
        for (size_t i = 0; i < n; i++) {
            int16_t v = g711_ulaw_decode(ulaw[i]);
            pcm[2 * (done + i)] = (uint8_t)(v & 0xFF);
            pcm[2 * (done + i) + 1] = (uint8_t)((uint16_t)v >> 8);
        }
        done += n;
        if (n < k) {
            break;
        }
    }
    if (done < want) {
        s_playing = false;  /* 바닥났다 → 다시 모은다 */
        if (!s_turn_done) {
            s_underruns++;  /* 말하는 도중에 음성이 늦게 왔다 (네트워크가 못 따라옴) */
            size_t next = s_prebuffer + PREBUFFER_STEP;
            s_prebuffer = next < PREBUFFER_MAX ? next : PREBUFFER_MAX;  /* 지연↑ 끊김↓ */
        }
    }
    return done * 2;
}

void realtime_get_stats(realtime_stats_t *st)
{
    st->rx_bytes = s_rx_bytes;
    st->underruns = s_underruns;
    st->send_fails = s_send_fails;
    st->ai_buffered_ms = (uint32_t)(pcm_ring_count(&s_ai_audio) / ULAW_PER_MS);
    st->prebuffer_ms = (uint32_t)(s_prebuffer / ULAW_PER_MS);
}

size_t realtime_pending_ai_audio(void)
{
    return pcm_ring_count(&s_ai_audio);
}

void realtime_stop(void)
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
        ESP_LOGI(TAG, "session closed — 토큰: 입력 음성 %u·글 %u (그중 캐시 %u), 출력 음성 %u·글 %u",
                 (unsigned)s_usage.in_audio, (unsigned)s_usage.in_text, (unsigned)s_usage.in_cached,
                 (unsigned)s_usage.out_audio, (unsigned)s_usage.out_text);
    }
    pcm_ring_clear(&s_ai_audio);
}
