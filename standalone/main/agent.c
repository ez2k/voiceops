#include "agent.h"

#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "gemini.h"
#include "phone.h"
#include "wifi.h"

#define TAG                 "agent"
#define DEVICE_NAME         "AI CallBridge"
#define HANDOFF_TIMEOUT_MS  15000   /* 끊기·인계 전에 AI 음성이 다 나가기를 기다리는 최대 시간 */
#define HEAP_LOG_MS         5000
#define RING_PERIOD_MS      3000    /* 설정의 "벨 N번" = N × 3초 (폰마다 RING 신호가 달라 시간으로 센다) */
#define TICK_MS             200

typedef enum {
    MSG_PHONE,
    MSG_END_CALL,
    MSG_TRANSFER,
    MSG_AI_FAILED,
} msg_kind_t;

typedef struct {
    msg_kind_t kind;
    phone_event_type_t evt;
    int value;
    char text[40];
} agent_msg_t;

static QueueHandle_t s_queue;
static const settings_t *s_settings;
static volatile bool s_ai_call;  /* AI 가 지금 통화 음성을 맡고 있다 (블루투스 태스크가 읽는다) */

/* 이번 전화 상태 (agent 태스크만 만진다) */
static bool s_incoming;          /* 전화가 울리는 중 */
static TickType_t s_incoming_since;
static bool s_answer_tried;
static int s_rings;              /* 폰이 보낸 RING 신호 수 (진단용) */
static bool s_answered;          /* AI 가 받은 전화다 */
static bool s_call_active;       /* 통화가 연결돼 있다 */
static bool s_audio_on;          /* 통화 음성 채널이 열려 있다 (벨소리를 위해 받기 전에 열리기도 한다) */
static int s_audio_rate;
static char s_number[32];

static void post(const agent_msg_t *m)
{
    if (xQueueSend(s_queue, m, 0) != pdTRUE) {
        ESP_LOGW(TAG, "event queue full");
    }
}

/* ---- 콜백 (다른 태스크) → 큐 ---- */

static void on_phone_event(const phone_event_t *e)
{
    agent_msg_t m = {.kind = MSG_PHONE, .evt = e->type, .value = e->value};
    if (e->text != NULL) {
        strlcpy(m.text, e->text, sizeof(m.text));
    }
    post(&m);
}

static void on_phone_rx_audio(const uint8_t *pcm, size_t len)
{
    if (s_ai_call) {
        gemini_push_caller_audio(pcm, len);
    }
}

static size_t fill_phone_tx_audio(uint8_t *pcm, size_t len)
{
    return s_ai_call ? gemini_pull_ai_audio(pcm, len) : 0;
}

static void on_ai_end_call(void)
{
    const agent_msg_t m = {.kind = MSG_END_CALL};
    post(&m);
}

static void on_ai_transfer(const char *reason)
{
    agent_msg_t m = {.kind = MSG_TRANSFER};
    strlcpy(m.text, reason, sizeof(m.text));
    post(&m);
}

static void on_ai_failed(const char *why)
{
    agent_msg_t m = {.kind = MSG_AI_FAILED};
    strlcpy(m.text, why, sizeof(m.text));
    post(&m);
}

/* ---- 통화 흐름 (agent 태스크) ---- */

static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap %s: free=%u min=%u", when,
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
}

static void reset_call(void)
{
    s_incoming = false;
    s_answer_tried = false;
    s_rings = 0;
    s_answered = false;
    s_number[0] = '\0';
}

/* AI 가 받은 통화이고 음성 채널이 열려 있으면 AI 음성을 흘린다. */
static void start_ai_audio(void)
{
    if (s_answered && s_audio_on && !s_ai_call) {
        gemini_audio_start(s_audio_rate);
        s_ai_call = true;
        ESP_LOGI(TAG, "AI 응대 시작");
        log_heap("call start");
    }
}

static void try_answer(void)
{
    if (s_settings->api_key[0] == '\0') {
        ESP_LOGW(TAG, "Gemini API 키가 없어 AI 가 받지 않습니다");
        return;
    }
    if (!wifi_connected()) {
        ESP_LOGW(TAG, "Wi-Fi 가 연결되지 않아 AI 가 받지 않습니다 — 사람이 받아 주세요");
        return;
    }
    esp_err_t err = gemini_start(s_number);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI 세션을 열지 못했습니다: %s", esp_err_to_name(err));
        return;
    }
    err = phone_answer();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "전화를 받지 못했습니다: %s", esp_err_to_name(err));
        gemini_stop();
        return;
    }
    s_answered = true;
    s_incoming = false;
    ESP_LOGI(TAG, "AI 가 전화를 받습니다");
    start_ai_audio();  /* 벨소리 때문에 음성 채널이 이미 열려 있으면 바로 시작 */
}

/* 벨이 설정한 시간만큼 울리면 AI 가 받는다 (한 통화에 한 번만 시도). */
static void check_auto_answer(void)
{
    if (s_incoming && !s_answered && !s_answer_tried &&
        xTaskGetTickCount() - s_incoming_since >= pdMS_TO_TICKS(s_settings->rings * RING_PERIOD_MS)) {
        s_answer_tried = true;
        try_answer();
    }
}

/* AI 가 하던 말(작별 인사, "연결해 드리겠습니다")이 다 나갈 때까지 기다린다. */
static void wait_ai_quiet(void)
{
    const TickType_t start = xTaskGetTickCount();
    vTaskDelay(pdMS_TO_TICKS(500));
    while (gemini_pending_ai_audio() > 0 && xTaskGetTickCount() - start < pdMS_TO_TICKS(HANDOFF_TIMEOUT_MS)) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(300));
}

/* AI 응대를 멈추고 소리를 폰 본체로 넘긴다 (통화는 유지). */
static void hand_over(void)
{
    s_answered = false;
    s_ai_call = false;
    phone_audio_disconnect();
    gemini_stop();
}

static void handle_phone(const agent_msg_t *m)
{
    switch (m->evt) {
    case PHONE_EVT_READY:
        ESP_LOGI(TAG, "블루투스 준비됨 (%s)", m->text);
        break;
    case PHONE_EVT_PAIRING:
        if (m->value) {
            ESP_LOGI(TAG, "페어링 모드: 폰의 블루투스 설정에서 \"" DEVICE_NAME "\" 를 선택하세요");
        }
        break;
    case PHONE_EVT_PAIRED:
        ESP_LOGI(TAG, "페어링 완료: %s", m->text);
        break;
    case PHONE_EVT_PAIR_FAILED:
        ESP_LOGW(TAG, "페어링 실패: %s (%d)", m->text, m->value);
        break;
    case PHONE_EVT_CONN:
        ESP_LOGI(TAG, "폰 연결 상태: %s %s", phone_conn_state_name(m->value), m->text);
        break;
    case PHONE_EVT_CALLSETUP:
        ESP_LOGI(TAG, "callsetup=%d", m->value);
        if (m->value == 1) {
            reset_call();
            s_incoming = true;
            s_incoming_since = xTaskGetTickCount();
            ESP_LOGI(TAG, "전화가 옵니다 (%d초 뒤 AI 가 받습니다)", s_settings->rings * RING_PERIOD_MS / 1000);
        } else if (m->value == 0) {
            s_incoming = false;  /* 받았거나, 끊겼거나 */
        }
        break;
    case PHONE_EVT_CLIP:
        strlcpy(s_number, m->text, sizeof(s_number));
        ESP_LOGI(TAG, "발신 번호: %s", s_number);
        break;
    case PHONE_EVT_RING:
        ESP_LOGI(TAG, "벨 %d", ++s_rings);
        break;
    case PHONE_EVT_CALL:
        s_call_active = m->value != 0;
        if (s_call_active) {
            ESP_LOGI(TAG, "통화 연결");
            if (!s_answered && s_audio_on) {
                ESP_LOGI(TAG, "사람이 받은 통화 — 소리를 폰 본체로 돌립니다");
                phone_audio_disconnect();
            }
        } else {
            if (s_answered || s_ai_call) {
                s_ai_call = false;
                gemini_stop();
            }
            reset_call();
            ESP_LOGI(TAG, "통화 끝");
        }
        break;
    case PHONE_EVT_AUDIO_ON:
        s_audio_on = true;
        s_audio_rate = m->value;
        if (s_answered) {
            start_ai_audio();
        } else if (s_call_active) {
            ESP_LOGI(TAG, "사람이 받은 통화 — 소리를 폰 본체로 돌립니다");
            phone_audio_disconnect();
        } else {
            ESP_LOGI(TAG, "음성 채널 열림 (벨소리) — 받을 때까지 대기");
        }
        break;
    case PHONE_EVT_AUDIO_OFF:
        /* 세션은 통화가 끝날 때 닫는다. 받는 순간 폰이 음성 채널을 다시 여는 경우가 있다. */
        s_audio_on = false;
        s_ai_call = false;
        break;
    case PHONE_EVT_AUDIO_UNSUPPORTED:
        ESP_LOGE(TAG, "지원하지 않는 통화 코덱: %s", m->text);
        break;
    default:
        break;
    }
}

static void agent_task(void *arg)
{
    agent_msg_t m;
    TickType_t last_heap_log = xTaskGetTickCount();
    for (;;) {
        bool got = xQueueReceive(s_queue, &m, pdMS_TO_TICKS(TICK_MS)) == pdTRUE;
        check_auto_answer();
        if (s_ai_call && xTaskGetTickCount() - last_heap_log >= pdMS_TO_TICKS(HEAP_LOG_MS)) {
            last_heap_log = xTaskGetTickCount();
            log_heap("during call");
        }
        if (!got) {
            continue;
        }
        switch (m.kind) {
        case MSG_PHONE:
            handle_phone(&m);
            break;
        case MSG_END_CALL:
            ESP_LOGI(TAG, "AI 가 통화를 끝냅니다");
            wait_ai_quiet();
            phone_hangup();
            break;
        case MSG_TRANSFER:
            ESP_LOGW(TAG, "사람에게 인계 (%s) — 폰을 들고 이어서 통화하세요", m.text);
            wait_ai_quiet();
            hand_over();
            break;
        case MSG_AI_FAILED:
            if (s_answered || s_ai_call) {
                ESP_LOGE(TAG, "AI 연결 문제 (%s) — 사람에게 인계합니다", m.text);
                hand_over();
            }
            break;
        }
    }
}

void agent_start(const settings_t *settings)
{
    s_settings = settings;
    s_queue = xQueueCreate(32, sizeof(agent_msg_t));

    const phone_callbacks_t phone_cb = {
        .on_event = on_phone_event,
        .on_rx_audio = on_phone_rx_audio,
        .fill_tx_audio = fill_phone_tx_audio,
    };
    phone_start(&phone_cb, DEVICE_NAME);
    log_heap("after bluetooth");

    const gemini_callbacks_t gemini_cb = {
        .on_end_call = on_ai_end_call,
        .on_transfer = on_ai_transfer,
        .on_failed = on_ai_failed,
    };
    const gemini_config_t gemini_cfg = {
        .api_key = settings->api_key,
        .model = settings->model,
        .prompt = settings->prompt,
    };
    ESP_ERROR_CHECK(gemini_init(&gemini_cb, &gemini_cfg));
    log_heap("after gemini buffers");

    xTaskCreate(agent_task, "agent", 4096, NULL, 4, NULL);
}
