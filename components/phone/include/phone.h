/*
 * 휴대폰 쪽: 블루투스 HFP 핸즈프리 (차량용 카킷 역할) — 공용 컴포넌트
 *
 * 폰과 페어링·연결을 유지하고, 통화 상태를 이벤트로 알리며, 통화 음성(SCO)을 PCM 으로 주고받는다.
 * 무엇을 할지는 앱(usb-bridge, standalone)이 콜백으로 정한다.
 *
 * 1단계는 CVSD(8kHz 협대역)만 쓴다. ESP32 컨트롤러가 CVSD <-> PCM 변환을 해 준다.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
    PHONE_EVT_READY,              /* text = 내 블루투스 주소 */
    PHONE_EVT_PAIRING,            /* value = 1 페어링 모드 켜짐 / 0 꺼짐 */
    PHONE_EVT_PAIRED,             /* text = 폰 주소, text2 = 폰 이름 */
    PHONE_EVT_PAIR_FAILED,        /* text = 폰 주소, value = 상태 코드 */
    PHONE_EVT_CONN,               /* value = esp_hf_client_connection_state_t, text = 폰 주소 */
    PHONE_EVT_RING,
    PHONE_EVT_CLIP,               /* text = 발신 번호 */
    PHONE_EVT_WAITING,            /* text = 대기 중인 전화 번호 */
    PHONE_EVT_CALL,               /* value = 0 통화 없음 / 1 통화 중 */
    PHONE_EVT_CALLSETUP,          /* value = 0 없음 / 1 착신 / 2 발신 중 / 3 상대 울림 */
    PHONE_EVT_HELD,               /* value = 보류 상태 */
    PHONE_EVT_AUDIO_ON,           /* value = 샘플레이트(Hz), value2 = 프레임 바이트 */
    PHONE_EVT_AUDIO_OFF,
    PHONE_EVT_AUDIO_UNSUPPORTED,  /* text = 코덱 이름 */
    PHONE_EVT_SERVICE,            /* value = 0/1 */
    PHONE_EVT_SIGNAL,             /* value = 0-5 */
    PHONE_EVT_BATTERY,            /* value = 0-5 */
    PHONE_EVT_AT,                 /* value = AT 응답 코드, value2 = CME 오류 */
} phone_event_type_t;

typedef struct {
    phone_event_type_t type;
    int value;
    int value2;
    const char *text;   /* 콜백 안에서만 유효 */
    const char *text2;
} phone_event_t;

typedef struct {
    /* 블루투스 태스크에서 불린다. 오래 막지 말 것. */
    void (*on_event)(const phone_event_t *evt);
    /* 상대 목소리 PCM 16bit LE mono. SCO 프레임마다 블루투스 태스크에서 불린다. */
    void (*on_rx_audio)(const uint8_t *pcm, size_t len);
    /* 폰으로 보낼 우리 쪽 목소리를 pcm 에 채우고 채운 바이트 수를 돌려준다 (모자란 부분은 무음). */
    size_t (*fill_tx_audio)(uint8_t *pcm, size_t len);
} phone_callbacks_t;

typedef struct {
    int conn_state;     /* esp_hf_client_connection_state_t */
    bool have_peer;
    char peer[18];      /* 마지막으로 연결된 폰 주소 */
    bool audio;
    bool pairing;
} phone_status_t;

/* 블루투스 스택과 HFP 핸즈프리를 시작한다. nvs_flash_init() 이후에 부른다. */
void phone_start(const phone_callbacks_t *callbacks, const char *device_name);

esp_err_t phone_answer(void);
esp_err_t phone_hangup(void);                /* 걸려오는 전화 거절, 통화 종료 모두 */
esp_err_t phone_dial(const char *number);    /* 0-9 + * # 만 허용 */
esp_err_t phone_dtmf(const char *digits);    /* 0-9 * # A-D 만 허용 */
esp_err_t phone_audio_connect(void);         /* 소리를 다시 이 기기로 */
esp_err_t phone_audio_disconnect(void);      /* 통화는 유지하고 소리만 폰 본체로 (사람에게 인계) */
void phone_pair(void);                       /* 새 폰 페어링 허용 */
void phone_get_status(phone_status_t *status);
const char *phone_conn_state_name(int state);
