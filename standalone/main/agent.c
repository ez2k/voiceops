#include "agent.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "realtime.h"
#include "phone.h"
#include "wifi.h"

#define TAG                 "agent"
#define DEVICE_NAME         "AI CallBridge"
#define HANDOFF_TIMEOUT_MS  15000   /* 끊기·인계 전에 AI 음성이 다 나가기를 기다리는 최대 시간 */
#define HEAP_LOG_MS         5000
#define RING_PERIOD_MS      3000    /* 설정의 "벨 N번" = N × 3초 (폰마다 RING 신호가 달라 시간으로 센다) */
#define TICK_MS             200
#define AUDIO_WAIT_MS       2000    /* AI 가 받았는데 폰이 음성 채널을 이만큼 안 열면 직접 연다 */
#define SPEED_TEST_MS       20000   /* 속도 시험 시간 (AI 에게 30초쯤 길게 말하게 한다) */
#define TEST_AUDIO_WAIT_MS  5000    /* 통화 흉내 시험: 폰이 음성 채널을 열어 주기를 기다리는 시간 */
#define SPEED_WINDOW_TICKS  (1000 / TICK_MS)

typedef enum {
    MSG_PHONE,
    MSG_END_CALL,
    MSG_TRANSFER,
    MSG_AI_FAILED,
    MSG_SPEED_TEST,
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
static TickType_t s_answered_at;
static bool s_audio_requested;   /* 받은 뒤 음성 채널을 직접 열어 달라고 했다 (한 번만) */
static bool s_outgoing;          /* 사람이 폰에서 거는 전화 (AI 는 끼지 않는다) */
static bool s_call_active;       /* 통화가 연결돼 있다 */
static bool s_audio_on;          /* 통화 음성 채널이 열려 있다 (벨소리를 위해 받기 전에 열리기도 한다) */
static int s_audio_rate;
static char s_number[32];

/* 속도 시험 (agent 태스크만 만진다) */
typedef enum {
    TEST_OFF,
    TEST_RADIO,       /* 전화 없이 받기만 (소리는 버림) */
    TEST_CALL_WAIT,   /* 통화 흉내: 폰이 음성 채널을 열어 주기를 기다리는 중 */
    TEST_CALL,        /* 통화 흉내: 음성 채널을 열고 실제 통화처럼 AI 음성을 폰으로 흘림 */
} test_state_t;
static test_state_t s_test;
static TickType_t s_test_started;
static uint32_t s_test_hist[SPEED_WINDOW_TICKS];  /* 1초 동안의 누적 수신량 → 1초 최고 속도 */
static unsigned s_test_idx;
static uint32_t s_test_peak;

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
        realtime_push_caller_audio(pcm, len);
    }
}

static size_t fill_phone_tx_audio(uint8_t *pcm, size_t len)
{
    return s_ai_call ? realtime_pull_ai_audio(pcm, len) : 0;
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
    s_audio_requested = false;
    s_outgoing = false;
    s_number[0] = '\0';
}

/* AI 가 받은 통화이고 음성 채널이 열려 있으면 AI 음성을 흘린다. */
static void start_ai_audio(void)
{
    if (s_answered && s_audio_on && !s_ai_call) {
        realtime_audio_start(s_audio_rate);
        s_ai_call = true;
        ESP_LOGI(TAG, "AI 응대 시작");
        log_heap("call start");
    }
}

static void try_answer(void)
{
    if (s_settings->api_key[0] == '\0') {
        ESP_LOGW(TAG, "OpenAI API 키가 없어 AI 가 받지 않습니다");
        return;
    }
    if (!wifi_connected()) {
        ESP_LOGW(TAG, "Wi-Fi 가 연결되지 않아 AI 가 받지 않습니다 — 사람이 받아 주세요");
        return;
    }
    esp_err_t err = realtime_start(s_number, RT_OPEN_GREETING);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "AI 세션을 열지 못했습니다: %s", esp_err_to_name(err));
        return;
    }
    err = phone_answer();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "전화를 받지 못했습니다: %s", esp_err_to_name(err));
        realtime_stop();
        return;
    }
    s_answered = true;
    s_answered_at = xTaskGetTickCount();
    s_incoming = false;
    ESP_LOGI(TAG, "AI 가 전화를 받습니다");
    start_ai_audio();  /* 벨소리 때문에 음성 채널이 이미 열려 있으면 바로 시작 */
}

/* 받은 뒤 폰이 소리를 본체에 둔 채 음성 채널을 안 여는 경우가 있다 → 한 번 직접 연다.
 * (통화 중에 사람이 폰에서 소리를 본체로 돌린 것은 인계로 보고 다시 가져오지 않는다) */
static void check_ai_audio(void)
{
    if (s_answered && s_call_active && !s_audio_on && !s_audio_requested &&
        xTaskGetTickCount() - s_answered_at >= pdMS_TO_TICKS(AUDIO_WAIT_MS)) {
        s_audio_requested = true;
        ESP_LOGW(TAG, "폰이 음성 채널을 열지 않아 직접 엽니다");
        phone_audio_connect();
    }
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
    while (realtime_pending_ai_audio() > 0 && xTaskGetTickCount() - start < pdMS_TO_TICKS(HANDOFF_TIMEOUT_MS)) {
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
    realtime_stop();
}

/* ---- 속도 시험 ----
 * radio: 블루투스 통화 없이 서버에서 AI 음성을 얼마나 빨리 받는지 잰다 (소리는 버림).
 * call : 전화 없이 폰의 통화 음성 채널(블루투스 SCO)만 열고, 실제 통화와 똑같이 AI 음성을 폰으로 흘린다.
 *        블루투스 통화가 무선을 차지하는 상황을 전화 없이 재현한다 (폰이 통화 없이 채널을 열어 줘야 한다).
 * 두 시험 모두 AI 에게 30초쯤 길게 말하게 해서 끊김이 잘 드러나게 한다. */

static void begin_measure(void)
{
    s_test_started = xTaskGetTickCount();
    memset(s_test_hist, 0, sizeof(s_test_hist));
    s_test_idx = 0;
    s_test_peak = 0;
}

static void stop_speed_test(const char *why)
{
    realtime_stats_t st;
    realtime_get_stats(&st);
    const bool call_like = s_test == TEST_CALL || s_test == TEST_CALL_WAIT;
    s_ai_call = false;
    realtime_stop();
    if (call_like) {
        phone_audio_disconnect();
        ESP_LOGI(TAG, "통화 흉내 시험 끝 (%s, 무선 우선 %s): 모두 %uKB, 1초 최고 %uKB/s, 끊김 %u번, 버퍼 %ums, "
                 "보내기 실패 %u번 — 끊김 0 이어야 합니다",
                 why, s_settings->coex, (unsigned)(st.rx_bytes / 1024), (unsigned)(s_test_peak / 1024),
                 (unsigned)st.underruns, (unsigned)st.prebuffer_ms, (unsigned)st.send_fails);
    } else {
        ESP_LOGI(TAG, "속도 시험 끝 (%s): 모두 %uKB, 1초 최고 %uKB/s — AI 음성은 실시간으로 약 11KB/s",
                 why, (unsigned)(st.rx_bytes / 1024), (unsigned)(s_test_peak / 1024));
    }
    s_test = TEST_OFF;
}

static void start_speed_test(bool call_like)
{
    if (s_test != TEST_OFF || s_incoming || s_call_active || s_answered) {
        ESP_LOGW(TAG, "통화 중이거나 이미 시험 중이라 시험하지 않습니다");
        return;
    }
    if (s_settings->api_key[0] == '\0' || !wifi_connected()) {
        ESP_LOGW(TAG, "시험에는 Wi-Fi 연결과 API 키가 필요합니다");
        return;
    }
    if (call_like) {
        esp_err_t err = phone_audio_connect();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "통화 흉내 시험: 폰과 연결돼 있지 않습니다 (%s)", esp_err_to_name(err));
            return;
        }
        s_test = TEST_CALL_WAIT;
        s_test_started = xTaskGetTickCount();
        ESP_LOGI(TAG, "통화 흉내 시험: 폰에 통화 음성 채널을 열어 달라고 했습니다");
        return;
    }
    esp_err_t err = realtime_start("", RT_OPEN_SPEED_TEST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "속도 시험: AI 세션을 열지 못했습니다: %s", esp_err_to_name(err));
        return;
    }
    s_test = TEST_RADIO;
    begin_measure();
    ESP_LOGI(TAG, "속도 시험 시작: 전화 없이 AI 음성을 %d초 동안 받습니다", SPEED_TEST_MS / 1000);
}

/* 통화 흉내 시험: 음성 채널이 열렸다 → 실제 통화와 같은 경로로 AI 를 붙인다 */
static void on_test_audio_on(int rate)
{
    esp_err_t err = realtime_start("", RT_OPEN_SPEED_TEST);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "통화 흉내 시험: AI 세션을 열지 못했습니다: %s", esp_err_to_name(err));
        stop_speed_test("AI 세션 실패");
        return;
    }
    realtime_audio_start(rate);
    s_ai_call = true;
    s_test = TEST_CALL;
    begin_measure();
    ESP_LOGI(TAG, "통화 흉내 시험 시작: 음성 채널이 열렸습니다 — %d초 동안 실제 통화처럼 AI 음성을 흘립니다",
             SPEED_TEST_MS / 1000);
}

static void run_speed_test(void)
{
    if (s_test == TEST_OFF) {
        return;
    }
    if (s_test == TEST_CALL_WAIT) {
        if (xTaskGetTickCount() - s_test_started >= pdMS_TO_TICKS(TEST_AUDIO_WAIT_MS)) {
            stop_speed_test("폰이 통화 없이 음성 채널을 열어 주지 않음");
        }
        return;
    }
    if (s_test == TEST_RADIO) {
        static uint8_t sink[1024];
        while (realtime_pull_ai_audio(sink, sizeof(sink)) > 0) {
            /* 소리는 버린다 — 받는 속도만 잰다 (통화 흉내 시험은 블루투스가 실시간으로 꺼내 간다) */
        }
    }
    realtime_stats_t st;
    realtime_get_stats(&st);
    uint32_t window = st.rx_bytes - s_test_hist[s_test_idx];  /* 지금 - 1초 전 */
    if (window > s_test_peak) {
        s_test_peak = window;
    }
    s_test_hist[s_test_idx] = st.rx_bytes;
    s_test_idx = (s_test_idx + 1) % SPEED_WINDOW_TICKS;
    if (xTaskGetTickCount() - s_test_started >= pdMS_TO_TICKS(SPEED_TEST_MS)) {
        stop_speed_test("시간 다 됨");
    }
}

void agent_speed_test(bool call_like)
{
    const agent_msg_t m = {.kind = MSG_SPEED_TEST, .value = call_like};
    post(&m);
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
            if (s_test != TEST_OFF) {
                stop_speed_test("전화가 와서 중단");
            }
            reset_call();
            s_incoming = true;
            s_incoming_since = xTaskGetTickCount();
            ESP_LOGI(TAG, "전화가 옵니다 (%d초 뒤 AI 가 받습니다)", s_settings->rings * RING_PERIOD_MS / 1000);
        } else if ((m->value == 2 || m->value == 3) && !s_outgoing) {
            /* 2 = 거는 중, 3 = 상대 쪽 벨이 울리는 중. 폰은 거는 전화 소리도 이 기기로 보내므로 바로 폰 본체로 돌린다
             * (그대로 두면 사람이 신호음을 못 듣는다) */
            reset_call();
            s_outgoing = true;
            ESP_LOGI(TAG, "폰에서 거는 전화 — 소리는 폰 본체로");
            if (s_audio_on) {
                phone_audio_disconnect();
            }
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
                realtime_stop();
            }
            reset_call();
            ESP_LOGI(TAG, "통화 끝");
        }
        break;
    case PHONE_EVT_AUDIO_ON:
        s_audio_on = true;
        s_audio_rate = m->value;
        if (s_test == TEST_CALL_WAIT) {
            on_test_audio_on(m->value);
        } else if (s_outgoing) {
            phone_audio_disconnect();
        } else if (s_answered) {
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
        if (s_test == TEST_CALL) {
            stop_speed_test("폰이 음성 채널을 닫음");
        }
        break;
    case PHONE_EVT_AUDIO_UNSUPPORTED:
        ESP_LOGE(TAG, "지원하지 않는 통화 코덱: %s", m->text);
        break;
    default:
        break;
    }
}

/* 통화 중 5초마다: 메모리, AI 서버 수신 속도, AI 음성 끊김 */
static void log_call_stats(void)
{
    static uint32_t last_rx;
    static TickType_t last_tick;
    realtime_stats_t st;
    realtime_get_stats(&st);
    TickType_t now = xTaskGetTickCount();
    uint32_t ms = (uint32_t)((now - last_tick) * portTICK_PERIOD_MS);
    uint32_t rx = st.rx_bytes >= last_rx ? st.rx_bytes - last_rx : st.rx_bytes;
    /* big = 가장 큰 연속 빈 블록 (TLS 가 서버 레코드 하나(최대 16KB)를 통째로 받을 자리) */
    phone_audio_stats_t sco;
    phone_get_audio_stats(&sco, true);
    ESP_LOGI(TAG, "call stats: heap free=%u min=%u big=%u | ai rx=%uKB/s buffered=%ums prebuf=%ums underruns=%u send_fails=%u",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
             (unsigned)(ms ? rx / ms : 0), (unsigned)st.ai_buffered_ms, (unsigned)st.prebuffer_ms,
             (unsigned)st.underruns, (unsigned)st.send_fails);
    /* 블루투스 구간: 정상이면 sco=16000B/s, bad=0, gap 은 수 ms, txfail=0 */
    ESP_LOGI(TAG, "sco stats: sco=%uB/s frames=%u bad=%u gap_max=%ums txfail=%u",
             (unsigned)(ms ? (uint64_t)sco.rx_bytes * 1000 / ms : 0), (unsigned)sco.rx_frames, (unsigned)sco.rx_bad,
             (unsigned)sco.max_gap_ms, (unsigned)sco.tx_fail);
    last_rx = st.rx_bytes;
    last_tick = now;
}

static void agent_task(void *arg)
{
    agent_msg_t m;
    TickType_t last_heap_log = xTaskGetTickCount();
    for (;;) {
        bool got = xQueueReceive(s_queue, &m, pdMS_TO_TICKS(TICK_MS)) == pdTRUE;
        check_auto_answer();
        check_ai_audio();
        run_speed_test();
        if (s_ai_call && xTaskGetTickCount() - last_heap_log >= pdMS_TO_TICKS(HEAP_LOG_MS)) {
            last_heap_log = xTaskGetTickCount();
            log_call_stats();
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
        case MSG_SPEED_TEST:
            start_speed_test(m.value != 0);
            break;
        case MSG_AI_FAILED:
            if (s_test != TEST_OFF) {
                stop_speed_test(m.text);
            } else if (s_answered || s_ai_call) {
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

    const realtime_callbacks_t rt_cb = {
        .on_end_call = on_ai_end_call,
        .on_transfer = on_ai_transfer,
        .on_failed = on_ai_failed,
    };
    const realtime_config_t rt_cfg = {
        .url = settings->ai_url,
        .api_key = settings->api_key,
        .model = settings->model,
        .prompt = settings->prompt,
    };
    ESP_ERROR_CHECK(realtime_init(&rt_cb, &rt_cfg));
    log_heap("after AI buffers");

    xTaskCreate(agent_task, "agent", 4096, NULL, 4, NULL);
}
