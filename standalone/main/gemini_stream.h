/*
 * Gemini Live 서버 메시지 스트리밍 처리기.
 *
 * 음성이 담긴 메시지는 수십 KB 라서 PSRAM 없는 ESP32 에 통째로 담을 수 없다.
 * 조각이 들어오는 대로 JSON 의 "data" 문자열(base64 음성)만 바로 풀어 콜백으로 넘기고,
 * 나머지 JSON 은 "data":"" 로 비운 채 작은 버퍼에 모아 메시지가 끝나면 돌려준다.
 * 그러면 turnComplete·interrupted·toolCall 같은 제어 정보는 일반 JSON 파서로 읽을 수 있다.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "b64.h"

typedef void (*gs_audio_cb_t)(const uint8_t *bytes, size_t len, void *ctx);

#define GS_AUDIO_CHUNK 96

typedef struct {
    int state;
    char key[8];
    int key_len;
    bool key_bad;
    char *rest;
    size_t rest_cap;
    size_t rest_len;
    bool rest_overflow;
    b64_decoder_t b64;
    uint8_t audio[GS_AUDIO_CHUNK];
    size_t audio_len;
    gs_audio_cb_t on_audio;
    void *ctx;
} gemini_stream_t;

void gs_init(gemini_stream_t *s, char *rest_buf, size_t rest_cap, gs_audio_cb_t on_audio, void *ctx);

/* 새 메시지를 시작한다. */
void gs_begin(gemini_stream_t *s);

/* 메시지 조각을 넣는다. 풀린 음성 바이트는 on_audio 로 나간다. */
void gs_feed(gemini_stream_t *s, const char *data, size_t len);

/* 메시지를 끝낸다. 음성을 뺀 JSON 문자열(NUL 종료)을 돌려준다. 버퍼가 넘쳤으면 NULL. */
const char *gs_end(gemini_stream_t *s);
