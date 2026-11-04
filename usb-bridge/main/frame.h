/*
 * 시리얼 프레임 코덱 (ESP-IDF 의존성 없음 — PC에서도 컴파일해 테스트한다)
 *
 *   A5 5A | type(1) | len(2, LE) | payload(len) | crc16(2, LE)
 *
 * crc16 = CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF), type·len·payload 에 대해 계산.
 * 파이썬 쪽 bridge/callbridge/frame.py 와 반드시 같은 규칙을 유지한다.
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define FRAME_MAGIC0       0xA5
#define FRAME_MAGIC1       0x5A
#define FRAME_MAX_PAYLOAD  1024
#define FRAME_OVERHEAD     7   /* magic 2 + type 1 + len 2 + crc 2 */

typedef enum {
    FRAME_EVT   = 0x01,  /* ESP32 -> PC : 상태 이벤트 (텍스트) */
    FRAME_LOG   = 0x02,  /* ESP32 -> PC : 로그 (텍스트) */
    FRAME_AUDIO = 0x03,  /* 양방향      : PCM 16bit LE mono */
    FRAME_CMD   = 0x10,  /* PC -> ESP32 : 명령 (텍스트) */
} frame_type_t;

uint16_t frame_crc16(const uint8_t *data, size_t len, uint16_t crc);

/* out 에 프레임을 만든다. 성공 시 프레임 전체 길이, 공간 부족/과대 payload 면 0. */
size_t frame_encode(uint8_t type, const uint8_t *payload, uint16_t len,
                    uint8_t *out, size_t out_size);

typedef struct {
    uint8_t state;
    uint8_t type;
    uint16_t len;
    uint16_t pos;
    uint16_t crc_rx;
    uint8_t payload[FRAME_MAX_PAYLOAD];
} frame_parser_t;

void frame_parser_reset(frame_parser_t *p);

/* 바이트 하나를 넣는다. 올바른 프레임이 완성되면 true — p->type, p->payload, p->len 이 유효하다.
 * 프레임 밖의 바이트(부트 메시지 등)와 CRC 가 틀린 프레임은 조용히 버린다. */
bool frame_parser_feed(frame_parser_t *p, uint8_t byte);
