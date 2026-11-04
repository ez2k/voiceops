#include "phone.h"

#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_hf_client_api.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"

#define TAG                    "phone"
#define NVS_NAMESPACE          "callbridge"
#define NVS_KEY_PEER           "peer"
#define RECONNECT_INTERVAL_US  (10 * 1000 * 1000)
#define CVSD_SAMPLE_RATE       8000
#define MAX_SCO_FRAME          240

static const char *const CONN_STATE_NAME[] = {
    [ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED]  = "disconnected",
    [ESP_HF_CLIENT_CONNECTION_STATE_CONNECTING]    = "connecting",
    [ESP_HF_CLIENT_CONNECTION_STATE_CONNECTED]     = "connected",
    [ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED] = "slc",
    [ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTING] = "disconnecting",
};

static phone_callbacks_t s_cb;
static esp_bd_addr_t s_peer;
static bool s_have_peer;
static bool s_pairing;
static volatile esp_hf_client_connection_state_t s_conn_state = ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED;
static volatile bool s_audio_active;
static uint16_t s_frame_size;
static esp_timer_handle_t s_reconnect_timer;
static const uint8_t s_silence[MAX_SCO_FRAME];

static void bda_to_str(const uint8_t *bda, char *out, size_t size)
{
    snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x",
             bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
}

const char *phone_conn_state_name(int state)
{
    return (state >= 0 && state <= ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTING) ? CONN_STATE_NAME[state] : "unknown";
}

static void emit(phone_event_type_t type, int value, int value2, const char *text, const char *text2)
{
    const phone_event_t evt = { .type = type, .value = value, .value2 = value2, .text = text, .text2 = text2 };
    s_cb.on_event(&evt);
}

/* ---- 마지막으로 연결된 폰 기억 (재부팅 후 자동 재연결용) ---- */

static void peer_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    size_t len = sizeof(s_peer);
    s_have_peer = (nvs_get_blob(h, NVS_KEY_PEER, s_peer, &len) == ESP_OK) && len == sizeof(s_peer);
    nvs_close(h);
}

static void peer_save(const uint8_t *bda)
{
    if (s_have_peer && memcmp(s_peer, bda, sizeof(s_peer)) == 0) {
        return;
    }
    memcpy(s_peer, bda, sizeof(s_peer));
    s_have_peer = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs open failed");
        return;
    }
    if (nvs_set_blob(h, NVS_KEY_PEER, s_peer, sizeof(s_peer)) != ESP_OK || nvs_commit(h) != ESP_OK) {
        ESP_LOGE(TAG, "peer save failed");
    }
    nvs_close(h);
}

/* ---- 페어링: 처음 켜졌거나 phone_pair() 일 때만 검색 허용 + 페어링 수락 ---- */

static void set_pairing(bool on)
{
    s_pairing = on;
    esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, on ? ESP_BT_GENERAL_DISCOVERABLE : ESP_BT_NON_DISCOVERABLE);
    emit(PHONE_EVT_PAIRING, on ? 1 : 0, 0, NULL, NULL);
}

static void gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_BT_GAP_AUTH_CMPL_EVT: {
        char mac[18];
        bda_to_str(param->auth_cmpl.bda, mac, sizeof(mac));
        if (param->auth_cmpl.stat == ESP_BT_STATUS_SUCCESS) {
            emit(PHONE_EVT_PAIRED, 0, 0, mac, (const char *)param->auth_cmpl.device_name);
            if (s_pairing) {
                set_pairing(false);
            }
        } else {
            emit(PHONE_EVT_PAIR_FAILED, param->auth_cmpl.stat, 0, mac, NULL);
        }
        break;
    }
    case ESP_BT_GAP_CFM_REQ_EVT:
        /* Just Works 페어링. 페어링 모드가 아닐 때 낯선 기기가 붙는 것을 막는다. */
        esp_bt_gap_ssp_confirm_reply(param->cfm_req.bda, s_pairing);
        break;
    case ESP_BT_GAP_PIN_REQ_EVT: {
        esp_bt_pin_code_t pin = {'0', '0', '0', '0'};
        esp_bt_gap_pin_reply(param->pin_req.bda, s_pairing, 4, pin);
        break;
    }
    default:
        break;
    }
}

/* ---- 통화 음성 ----
 * 폰에서 음성 프레임이 하나 올 때마다 앱에 넘기고, 같은 길이만큼 앱의 음성을 폰으로 보낸다.
 * 송신 타이밍을 수신 프레임에 맞추므로 별도 타이머 없이 SCO 속도와 정확히 맞는다. */

static void on_sco_data(esp_hf_sync_conn_hdl_t hdl, esp_hf_audio_buff_t *buf, bool is_bad)
{
    bool good = (buf != NULL) && !is_bad && buf->data_len > 0;
    uint16_t n = s_frame_size;

    if (good) {
        n = buf->data_len;
        s_frame_size = n;
        s_cb.on_rx_audio(buf->data, n);
    }
    if (buf != NULL) {
        esp_hf_client_audio_buff_free(buf);
    }
    if (n == 0) {
        return;
    }
    if (!good) {
        /* 깨진 프레임은 무음으로 채워 앱 쪽 시간축(녹음 길이)을 유지한다. */
        s_cb.on_rx_audio(s_silence, n < MAX_SCO_FRAME ? n : MAX_SCO_FRAME);
    }
    if (!s_audio_active) {
        return;
    }

    esp_hf_audio_buff_t *out = esp_hf_client_audio_buff_alloc(n);
    if (out == NULL) {
        return;
    }
    size_t got = s_cb.fill_tx_audio(out->data, n);
    if (got < n) {
        memset(out->data + got, 0, n - got);
    }
    out->data_len = n;
    if (esp_hf_client_audio_data_send(hdl, out) != ESP_OK) {
        esp_hf_client_audio_buff_free(out);
    }
}

/* ---- HFP 이벤트 ---- */

static void hf_cb(esp_hf_client_cb_event_t event, esp_hf_client_cb_param_t *param)
{
    char mac[18];

    switch (event) {
    case ESP_HF_CLIENT_PROF_STATE_EVT:
        if (param->prof_stat.state == ESP_HF_INIT_SUCCESS && s_have_peer) {
            esp_hf_client_connect(s_peer);
        }
        break;

    case ESP_HF_CLIENT_CONNECTION_STATE_EVT:
        s_conn_state = param->conn_stat.state;
        bda_to_str(param->conn_stat.remote_bda, mac, sizeof(mac));
        if (param->conn_stat.state == ESP_HF_CLIENT_CONNECTION_STATE_SLC_CONNECTED) {
            peer_save(param->conn_stat.remote_bda);
        }
        emit(PHONE_EVT_CONN, param->conn_stat.state, 0, mac, NULL);
        break;

    case ESP_HF_CLIENT_AUDIO_STATE_EVT:
        switch (param->audio_stat.state) {
        case ESP_HF_CLIENT_AUDIO_STATE_CONNECTED:
            s_frame_size = param->audio_stat.preferred_frame_size;
            esp_hf_client_register_audio_data_callback(on_sco_data);
            emit(PHONE_EVT_AUDIO_ON, CVSD_SAMPLE_RATE, s_frame_size, NULL, NULL);
            s_audio_active = true;
            break;
        case ESP_HF_CLIENT_AUDIO_STATE_CONNECTED_MSBC:
            /* sdkconfig 에서 Wide Band Speech 를 꺼 두었으므로 오지 않아야 한다. */
            ESP_LOGE(TAG, "mSBC negotiated but not supported yet");
            emit(PHONE_EVT_AUDIO_UNSUPPORTED, 0, 0, "msbc", NULL);
            break;
        case ESP_HF_CLIENT_AUDIO_STATE_DISCONNECTED:
            s_audio_active = false;
            emit(PHONE_EVT_AUDIO_OFF, 0, 0, NULL, NULL);
            break;
        default:
            break;
        }
        break;

    case ESP_HF_CLIENT_RING_IND_EVT:
        emit(PHONE_EVT_RING, 0, 0, NULL, NULL);
        break;
    case ESP_HF_CLIENT_CLIP_EVT:
        emit(PHONE_EVT_CLIP, 0, 0, param->clip.number ? param->clip.number : "", NULL);
        break;
    case ESP_HF_CLIENT_CCWA_EVT:
        emit(PHONE_EVT_WAITING, 0, 0, param->ccwa.number ? param->ccwa.number : "", NULL);
        break;
    case ESP_HF_CLIENT_CIND_CALL_EVT:
        emit(PHONE_EVT_CALL, (int)param->call.status, 0, NULL, NULL);
        break;
    case ESP_HF_CLIENT_CIND_CALL_SETUP_EVT:
        emit(PHONE_EVT_CALLSETUP, (int)param->call_setup.status, 0, NULL, NULL);
        break;
    case ESP_HF_CLIENT_CIND_CALL_HELD_EVT:
        emit(PHONE_EVT_HELD, (int)param->call_held.status, 0, NULL, NULL);
        break;
    case ESP_HF_CLIENT_CIND_SERVICE_AVAILABILITY_EVT:
        emit(PHONE_EVT_SERVICE, (int)param->service_availability.status, 0, NULL, NULL);
        break;
    case ESP_HF_CLIENT_CIND_SIGNAL_STRENGTH_EVT:
        emit(PHONE_EVT_SIGNAL, param->signal_strength.value, 0, NULL, NULL);
        break;
    case ESP_HF_CLIENT_CIND_BATTERY_LEVEL_EVT:
        emit(PHONE_EVT_BATTERY, param->battery_level.value, 0, NULL, NULL);
        break;
    case ESP_HF_CLIENT_AT_RESPONSE_EVT:
        emit(PHONE_EVT_AT, (int)param->at_response.code, (int)param->at_response.cme, NULL, NULL);
        break;
    default:
        break;
    }
}

/* 폰이 꺼졌다 켜지거나 멀어졌다 돌아와도 다시 붙도록 주기적으로 재연결을 시도한다. */
static void reconnect_cb(void *arg)
{
    if (s_have_peer && s_conn_state == ESP_HF_CLIENT_CONNECTION_STATE_DISCONNECTED) {
        ESP_LOGI(TAG, "reconnecting to phone");
        esp_hf_client_connect(s_peer);
    }
}

/* ---- 앱이 부르는 통화 조작 ---- */

static bool valid_chars(const char *s, const char *allowed)
{
    if (s == NULL || *s == '\0') {
        return false;
    }
    for (; *s; s++) {
        if (strchr(allowed, *s) == NULL) {
            return false;
        }
    }
    return true;
}

esp_err_t phone_answer(void)
{
    return esp_hf_client_answer_call();
}

esp_err_t phone_hangup(void)
{
    /* AT+CHUP: 걸려오는 전화 거절과 통화 종료 모두 이것으로 한다. */
    return esp_hf_client_reject_call();
}

esp_err_t phone_dial(const char *number)
{
    return valid_chars(number, "0123456789+*#") ? esp_hf_client_dial(number) : ESP_ERR_INVALID_ARG;
}

esp_err_t phone_dtmf(const char *digits)
{
    if (!valid_chars(digits, "0123456789*#ABCD")) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = ESP_OK;
    for (const char *c = digits; err == ESP_OK && *c; c++) {
        err = esp_hf_client_send_dtmf(*c);
    }
    return err;
}

esp_err_t phone_audio_connect(void)
{
    return s_have_peer ? esp_hf_client_connect_audio(s_peer) : ESP_ERR_INVALID_STATE;
}

esp_err_t phone_audio_disconnect(void)
{
    return s_have_peer ? esp_hf_client_disconnect_audio(s_peer) : ESP_ERR_INVALID_STATE;
}

void phone_pair(void)
{
    set_pairing(true);
}

void phone_get_status(phone_status_t *status)
{
    status->conn_state = s_conn_state;
    status->have_peer = s_have_peer;
    if (s_have_peer) {
        bda_to_str(s_peer, status->peer, sizeof(status->peer));
    } else {
        strcpy(status->peer, "none");
    }
    status->audio = s_audio_active;
    status->pairing = s_pairing;
}

void phone_start(const phone_callbacks_t *callbacks, const char *device_name)
{
    s_cb = *callbacks;
    peer_load();

    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
    ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT));
    esp_bluedroid_config_t bd_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_bluedroid_init_with_cfg(&bd_cfg));
    ESP_ERROR_CHECK(esp_bluedroid_enable());

    ESP_ERROR_CHECK(esp_bt_gap_register_callback(gap_cb));
    ESP_ERROR_CHECK(esp_bt_gap_set_device_name(device_name));
    esp_bt_io_cap_t iocap = ESP_BT_IO_CAP_NONE;
    ESP_ERROR_CHECK(esp_bt_gap_set_security_param(ESP_BT_SP_IOCAP_MODE, &iocap, sizeof(iocap)));

    ESP_ERROR_CHECK(esp_hf_client_register_callback(hf_cb));
    ESP_ERROR_CHECK(esp_hf_client_init());

    const esp_timer_create_args_t timer_args = {
        .callback = reconnect_cb,
        .name = "hf_reconnect",
    };
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &s_reconnect_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_reconnect_timer, RECONNECT_INTERVAL_US));

    set_pairing(esp_bt_gap_get_bond_device_num() == 0);

    char mac[18];
    bda_to_str(esp_bt_dev_get_address(), mac, sizeof(mac));
    emit(PHONE_EVT_READY, 0, 0, mac, NULL);
}
