/* 태스크 사이에서 PCM 을 넘기는 고리 버퍼. clear() 를 어느 쪽에서 불러도 안전하다 (AI 끼어들기 처리용). */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"

typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t head;   /* 다음에 읽을 위치 */
    size_t count;
    portMUX_TYPE lock;
} pcm_ring_t;

bool pcm_ring_init(pcm_ring_t *r, size_t cap);
size_t pcm_ring_write(pcm_ring_t *r, const uint8_t *data, size_t len);  /* 들어간 만큼 돌려준다 */
size_t pcm_ring_read(pcm_ring_t *r, uint8_t *out, size_t len);         /* 읽은 만큼 돌려준다 */
size_t pcm_ring_count(pcm_ring_t *r);
void pcm_ring_clear(pcm_ring_t *r);
