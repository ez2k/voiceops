/*
 * Gemini Live 음성 세션 (ESP32 에서 WebSocket 으로 직접 연결)
 *
 *   상대 음성 8kHz ──base64/JSON──> Gemini Live (서버가 16kHz 로 변환)
 *   Gemini 음성 24kHz ──스트리밍 해독·1/3 다운샘플──> 8kHz ──> 폰
 *
 * 통화 하나에 세션 하나: gemini_start() ~ gemini_stop().
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
} gemini_callbacks_t;

typedef struct {
    const char *api_key;
    const char *model;    /* 예: "gemini-3.8-live" */
    const char *prompt;   /* 지시문 (역할, 인사말, 업무 지식, 처리 규칙) */
} gemini_config_t;        /* 문자열은 프로그램이 끝날 때까지 유효해야 한다 */

/* 버퍼와 송신 태스크를 준비한다. 부팅 때 한 번 (메모리를 일찍 잡아 두려고). */
esp_err_t gemini_init(const gemini_callbacks_t *callbacks, const gemini_config_t *config);

/* 세션을 연다 (연결 → 설정 → 첫 인사). 응답을 기다리지 않고 바로 돌아온다. */
esp_err_t gemini_start(const char *caller_number);

/* 통화 음성이 연결됐다. 이때부터 상대 음성을 Gemini 로 보낸다. rate 는 8000 만 지원. */
void gemini_audio_start(int rate);

/* 상대 음성 (블루투스 태스크에서, 막지 않음) */
void gemini_push_caller_audio(const uint8_t *pcm, size_t len);

/* 폰으로 보낼 AI 음성을 꺼낸다 (블루투스 태스크에서, 막지 않음) */
size_t gemini_pull_ai_audio(uint8_t *pcm, size_t len);

/* 아직 폰으로 나가지 않은 AI 음성 바이트 수 */
size_t gemini_pending_ai_audio(void);

typedef struct {
    uint32_t rx_bytes;        /* 세션 시작 뒤 서버에서 받은 바이트 (AI 음성은 약 64KB/초 필요) */
    uint32_t underruns;       /* AI 가 말하는 도중 음성이 바닥난 횟수 (네트워크가 못 따라옴) */
    uint32_t send_fails;      /* 상대 음성 전송 실패 횟수 */
    uint32_t ai_buffered_ms;  /* 아직 폰으로 안 나간 AI 음성 */
    uint32_t prebuffer_ms;    /* 지터 버퍼 문턱 (끊길 때마다 늘어난다) */
} gemini_stats_t;

void gemini_get_stats(gemini_stats_t *stats);

/* 세션을 닫는다. WebSocket 태스크 안에서 부르면 안 된다. */
void gemini_stop(void);
