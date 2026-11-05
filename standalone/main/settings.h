/*
 * 기기 설정 (NVS 에 저장, 재부팅하면 적용)
 *
 * 웹 설정 페이지(web/index.html)나 시리얼 터미널에서 "cfg ..." 명령으로 바꾼다 (console.c).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define SETTINGS_PROMPT_MAX 8192

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    char ai_url[128];     /* AI 서버: OpenAI 직접 또는 직접 운영하는 중계 서버(relay/) — wss:// 또는 ws:// */
    char api_key[256];    /* OpenAI API 키 (sk-proj-… 는 160자 안팎) 또는 중계 서버 토큰 */
    char model[64];
    int rings;            /* 벨이 몇 번 울리면 AI 가 받을지 */
    char coex[8];         /* 무선 우선: balance | wifi | bt (블루투스 통화와 Wi-Fi 가 무선을 나눠 쓰는 비율) */
    char *prompt;         /* 지시문: 저장된 것이 없으면 펌웨어에 내장된 기본 지시문 */
    bool prompt_custom;
} settings_t;

void settings_load(settings_t *s);

/* key: wifi_ssid, wifi_pass, ai_url, api_key, model, rings, coex */
esp_err_t settings_set(const char *key, const char *value);

/* len 이 0 이면 저장된 지시문을 지우고 기본 지시문으로 돌아간다. */
esp_err_t settings_set_prompt(const char *text, size_t len);
