#include "link.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define LINK_UART        UART_NUM_0
#define LINK_TX_PIN      1   /* ESP32 UART0 기본 핀 (개발보드 USB-시리얼 칩에 연결됨) */
#define LINK_RX_PIN      3
#define LINK_RX_BUF      4096
#define LINK_TX_BUF      8192
#define LINK_TEXT_MAX    256
#define LINK_CMD_MAX     128

static SemaphoreHandle_t s_tx_lock;
static uint8_t s_tx_frame[FRAME_MAX_PAYLOAD + FRAME_OVERHEAD];
static link_cmd_handler_t s_on_cmd;
static link_audio_handler_t s_on_audio;

void link_send(frame_type_t type, const void *payload, size_t len)
{
    /* 이 함수 안에서는 ESP_LOG 를 쓰지 않는다 — 로그 훅이 다시 여기로 들어와 교착된다. */
    if (s_tx_lock == NULL) {
        return;
    }
    if (len > FRAME_MAX_PAYLOAD) {
        len = FRAME_MAX_PAYLOAD;
    }
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    size_t n = frame_encode((uint8_t)type, payload, (uint16_t)len, s_tx_frame, sizeof(s_tx_frame));
    if (n > 0) {
        uart_write_bytes(LINK_UART, s_tx_frame, n);
    }
    xSemaphoreGive(s_tx_lock);
}

static void send_text(frame_type_t type, const char *fmt, va_list args)
{
    char text[LINK_TEXT_MAX];
    int n = vsnprintf(text, sizeof(text), fmt, args);
    if (n <= 0) {
        return;
    }
    size_t len = (n < (int)sizeof(text)) ? (size_t)n : sizeof(text) - 1;
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }
    if (len > 0) {
        link_send(type, text, len);
    }
}

void link_eventf(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    send_text(FRAME_EVT, fmt, args);
    va_end(args);
}

static int log_to_link(const char *fmt, va_list args)
{
    send_text(FRAME_LOG, fmt, args);
    return 0;
}

static void rx_task(void *arg)
{
    static frame_parser_t parser;
    uint8_t buf[256];
    frame_parser_reset(&parser);

    for (;;) {
        int n = uart_read_bytes(LINK_UART, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < n; i++) {
            if (!frame_parser_feed(&parser, buf[i])) {
                continue;
            }
            if (parser.type == FRAME_CMD) {
                char cmd[LINK_CMD_MAX];
                size_t len = parser.len < sizeof(cmd) - 1 ? parser.len : sizeof(cmd) - 1;
                memcpy(cmd, parser.payload, len);
                cmd[len] = '\0';
                s_on_cmd(cmd);
            } else if (parser.type == FRAME_AUDIO) {
                s_on_audio(parser.payload, parser.len);
            }
        }
    }
}

void link_start(link_cmd_handler_t on_cmd, link_audio_handler_t on_audio)
{
    s_on_cmd = on_cmd;
    s_on_audio = on_audio;

    const uart_config_t cfg = {
        .baud_rate = LINK_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(LINK_UART, LINK_RX_BUF, LINK_TX_BUF, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(LINK_UART, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(LINK_UART, LINK_TX_PIN, LINK_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));

    s_tx_lock = xSemaphoreCreateMutex();
    esp_log_set_vprintf(log_to_link);

    xTaskCreate(rx_task, "link_rx", 4096, NULL, configMAX_PRIORITIES - 3, NULL);
}
