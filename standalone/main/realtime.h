/*
 * OpenAI Realtime 음성 세션 (ESP32 에서 WebSocket 으로 직접 연결)
 *
 *   상대 음성 8kHz PCM ──μ-law──base64/JSON──> OpenAI Realtime
 *   AI 음성 μ-law 8kHz ──스트리밍 해독──> 8kHz PCM ──> 폰
 *
 * 전화망과 같은 G.711 μ-law(1초 8KB)로 주고받는다. 블루투스 통화와 Wi-Fi 가 무선을 나눠 쓰는 동안
 * ESP32 가 받을 수 있는 속도(약 25KB/s)로도 충분하다 (24kHz 로만 보내는 Gemini Live 는 약 64KB/s 필요).
 *
 * 통화 하나에 세션 하나: realtime_start() ~ realtime_stop().
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef struct {
    /* 모두 WebSocket 태스크에서 불린다. 오래 막지 말고 큐로 넘길 것. */
    void (*on_end_call)(void);
    void (*on_transfer)(const char *reason);
    void (*on_failed)(const char *why);
} realtime_callbacks_t;

typedef struct {
    const char *url;      /* "wss://api.openai.com/v1/realtime" 또는 직접 운영하는 중계 서버 (relay/, Gemini) */
    const char *api_key;  /* Authorization: Bearer 로 보낸다 (OpenAI 키 또는 중계 서버 토큰) */
    const char *model;    /* 예: "gpt-realtime-2.1-mini" */
    const char *prompt;   /* 지시문 (역할, 인사말, 업무 지식, 처리 규칙) */
} realtime_config_t;      /* 문자열은 프로그램이 끝날 때까지 유효해야 한다 */

/* 버퍼와 송신 태스크를 준비한다. 부팅 때 한 번 (메모리를 일찍 잡아 두려고). */
esp_err_t realtime_init(const realtime_callbacks_t *callbacks, const realtime_config_t *config);

typedef enum {
    RT_OPEN_GREETING,     /* 전화 응대: AI 가 첫 인사를 한다 */
    RT_OPEN_SPEED_TEST,   /* 속도 시험: AI 가 30초쯤 길게 말한다 */
} rt_opening_t;

/* 세션을 연다 (연결 → 설정 → 첫 신호). 응답을 기다리지 않고 바로 돌아온다. */
esp_err_t realtime_start(const char *caller_number, rt_opening_t opening);

/* 통화 음성이 연결됐다. 이때부터 상대 음성을 보낸다. rate 는 8000 만 지원. */
void realtime_audio_start(int rate);

/* 상대 음성 8kHz 16bit PCM (블루투스 태스크에서, 막지 않음) */
void realtime_push_caller_audio(const uint8_t *pcm, size_t len);

/* 폰으로 보낼 AI 음성을 8kHz 16bit PCM 으로 꺼낸다 (블루투스 태스크에서, 막지 않음) */
size_t realtime_pull_ai_audio(uint8_t *pcm, size_t len);

/* 아직 폰으로 나가지 않은 AI 음성이 있는가 */
size_t realtime_pending_ai_audio(void);

typedef struct {
    uint32_t rx_bytes;        /* 세션 시작 뒤 서버에서 받은 바이트 (AI 음성은 약 11KB/초) */
    uint32_t underruns;       /* AI 가 말하는 도중 음성이 바닥난 횟수 (네트워크가 못 따라옴) */
    uint32_t send_fails;      /* 상대 음성 전송 실패 횟수 */
    uint32_t ai_buffered_ms;  /* 아직 폰으로 안 나간 AI 음성 */
    uint32_t prebuffer_ms;    /* 지터 버퍼 문턱 (끊길 때마다 늘어난다) */
} realtime_stats_t;

void realtime_get_stats(realtime_stats_t *stats);

/* 세션을 닫는다 (이번 세션의 토큰 사용량을 로그에 남긴다). WebSocket 태스크 안에서 부르면 안 된다. */
void realtime_stop(void);
