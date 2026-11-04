/*
 * AI CallBridge — USB 브리지 펌웨어 (ESP32 / ESP32-WROOM-32)
 *
 *   휴대폰 ⇄ 블루투스 HFP ⇄ ESP32 ⇄ USB 시리얼 ⇄ PC 브리지
 *
 * phone 컴포넌트의 이벤트를 텍스트 EVT 프레임으로 PC 에 알리고,
 * PC 의 CMD 프레임을 phone 조작으로 바꾸고, 통화 음성을 AUDIO 프레임으로 주고받는다.
 */
#include <string.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "nvs_flash.h"

#include "link.h"
#include "phone.h"

#define DEVICE_NAME     "AI CallBridge"
/* PC 가 보낸 송신 음성 버퍼 (8kHz 16bit 기준 0.5초). 넘치면 새로 온 데이터를 버린다. */
#define TX_BUFFER_BYTES 8000

static StreamBufferHandle_t s_tx_audio;
static volatile bool s_ready;

/* ---- phone -> PC ---- */

static void on_phone_event(const phone_event_t *e)
{
    switch (e->type) {
    case PHONE_EVT_READY:             link_eventf("ready %s", e->text); break;
    case PHONE_EVT_PAIRING:           link_eventf("pairing %s", e->value ? "on" : "off"); break;
    case PHONE_EVT_PAIRED:            link_eventf("paired %s %s", e->text, e->text2); break;
    case PHONE_EVT_PAIR_FAILED:       link_eventf("pair_failed %s %d", e->text, e->value); break;
    case PHONE_EVT_CONN:              link_eventf("conn %s %s", phone_conn_state_name(e->value), e->text); break;
    case PHONE_EVT_RING:              link_eventf("ring"); break;
    case PHONE_EVT_CLIP:              link_eventf("clip %s", e->text); break;
    case PHONE_EVT_WAITING:           link_eventf("waiting %s", e->text); break;
    case PHONE_EVT_CALL:              link_eventf("call %d", e->value); break;
    case PHONE_EVT_CALLSETUP:         link_eventf("callsetup %d", e->value); break;
    case PHONE_EVT_HELD:              link_eventf("held %d", e->value); break;
    case PHONE_EVT_AUDIO_ON:
        xStreamBufferReset(s_tx_audio);  /* 이전 통화의 남은 음성을 버린다 (송신 시작 전) */
        link_eventf("audio on cvsd %d %d", e->value, e->value2);
        break;
    case PHONE_EVT_AUDIO_OFF:         link_eventf("audio off"); break;
    case PHONE_EVT_AUDIO_UNSUPPORTED: link_eventf("audio unsupported %s", e->text); break;
    case PHONE_EVT_SERVICE:           link_eventf("service %d", e->value); break;
    case PHONE_EVT_SIGNAL:            link_eventf("signal %d", e->value); break;
    case PHONE_EVT_BATTERY:           link_eventf("battery %d", e->value); break;
    case PHONE_EVT_AT:                link_eventf("at %d %d", e->value, e->value2); break;
    }
}

static void on_phone_rx_audio(const uint8_t *pcm, size_t len)
{
    link_send(FRAME_AUDIO, pcm, len);
}

static size_t fill_phone_tx_audio(uint8_t *pcm, size_t len)
{
    return xStreamBufferReceive(s_tx_audio, pcm, len, 0);
}

/* ---- PC -> phone ---- */

static void on_host_audio(const uint8_t *pcm, size_t len)
{
    xStreamBufferSend(s_tx_audio, pcm, len, 0);
}

static void on_host_command(const char *cmd)
{
    if (!s_ready) {
        link_eventf("error %s not-ready", cmd);
        return;
    }

    esp_err_t err = ESP_OK;
    if (strcmp(cmd, "answer") == 0) {
        err = phone_answer();
    } else if (strcmp(cmd, "hangup") == 0) {
        err = phone_hangup();
    } else if (strncmp(cmd, "dial ", 5) == 0) {
        err = phone_dial(cmd + 5);
    } else if (strncmp(cmd, "dtmf ", 5) == 0) {
        err = phone_dtmf(cmd + 5);
    } else if (strcmp(cmd, "audio on") == 0) {
        err = phone_audio_connect();
    } else if (strcmp(cmd, "audio off") == 0) {
        err = phone_audio_disconnect();
    } else if (strcmp(cmd, "pair") == 0) {
        phone_pair();
    } else if (strcmp(cmd, "status") == 0) {
        phone_status_t st;
        phone_get_status(&st);
        link_eventf("status conn=%s peer=%s audio=%d pairing=%d",
                    phone_conn_state_name(st.conn_state), st.peer, st.audio ? 1 : 0, st.pairing ? 1 : 0);
    } else {
        link_eventf("error unknown-command %s", cmd);
        return;
    }
    if (err != ESP_OK) {
        link_eventf("error %s %s", cmd, esp_err_to_name(err));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    s_tx_audio = xStreamBufferCreate(TX_BUFFER_BYTES, 1);
    link_start(on_host_command, on_host_audio);

    const phone_callbacks_t callbacks = {
        .on_event = on_phone_event,
        .on_rx_audio = on_phone_rx_audio,
        .fill_tx_audio = fill_phone_tx_audio,
    };
    phone_start(&callbacks, DEVICE_NAME);
    s_ready = true;
}
