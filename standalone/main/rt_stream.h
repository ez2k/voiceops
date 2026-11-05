/*
 * Realtime 서버 메시지 스트리밍 처리기.
 *
 * 메시지 조각이 들어오는 대로 지정한 키(예: "delta")의 base64 문자열만 바로 풀어 콜백으로 넘기고,
 * 나머지 JSON 은 "delta":"" 로 비운 채 작은 버퍼에 모아 메시지가 끝나면 돌려준다.
 * 그러면 큰 음성 메시지를 통째로 담지 않고도 제어 정보는 일반 JSON 파서로 읽을 수 있다.
 *
 * 같은 키가 글자(자막 조각 등)에도 쓰이므로, 메시지 앞부분에 only_if 문자열
 * (예: "\"response.output_audio.delta\"")이 이미 나온 메시지에서만 음성으로 푼다.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "b64.h"

typedef void (*rs_audio_cb_t)(const uint8_t *bytes, size_t len, void *ctx);

#define RS_AUDIO_CHUNK 96
#define RS_KEY_MAX     16

typedef struct {
    int state;
    char key[RS_KEY_MAX];
    int key_len;
    bool key_bad;
    const char *audio_key;
    const char *only_if;
    char *rest;
    size_t rest_cap;
    size_t rest_len;
    bool rest_overflow;
    b64_decoder_t b64;
    uint8_t audio[RS_AUDIO_CHUNK];
    size_t audio_len;
    rs_audio_cb_t on_audio;
    void *ctx;
} rt_stream_t;

/* audio_key·only_if 는 프로그램이 끝날 때까지 유효해야 한다. only_if 가 NULL 이면 모든 메시지에서 푼다. */
void rs_init(rt_stream_t *s, char *rest_buf, size_t rest_cap, const char *audio_key, const char *only_if,
             rs_audio_cb_t on_audio, void *ctx);

/* 새 메시지를 시작한다. */
void rs_begin(rt_stream_t *s);

/* 메시지 조각을 넣는다. 풀린 음성 바이트는 on_audio 로 나간다. */
void rs_feed(rt_stream_t *s, const char *data, size_t len);

/* 메시지를 끝낸다. 음성을 뺀 JSON 문자열(NUL 종료)을 돌려준다. 버퍼가 넘쳤으면 NULL. */
const char *rs_end(rt_stream_t *s);
