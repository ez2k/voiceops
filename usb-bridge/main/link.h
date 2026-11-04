/*
 * PC 와의 시리얼 링크 (UART0 = 개발보드의 USB 포트)
 *
 * UART0 를 프레임 전용으로 쓴다. 콘솔은 꺼져 있고(CONFIG_ESP_CONSOLE_NONE),
 * ESP_LOG 출력은 LOG 프레임으로 감싸서 PC 로 보낸다.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "frame.h"

#define LINK_BAUD_RATE 921600

typedef void (*link_cmd_handler_t)(const char *cmd);
typedef void (*link_audio_handler_t)(const uint8_t *pcm, size_t len);

/* UART 를 열고 로그를 링크로 돌린 뒤, 수신 태스크를 시작한다. */
void link_start(link_cmd_handler_t on_cmd, link_audio_handler_t on_audio);

/* 프레임 하나를 보낸다. 여러 태스크에서 동시에 불러도 프레임이 섞이지 않는다. */
void link_send(frame_type_t type, const void *payload, size_t len);

/* EVT 프레임으로 텍스트 이벤트를 보낸다. 예: link_eventf("clip %s", number) */
void link_eventf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
