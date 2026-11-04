#include "console.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "b64.h"
#include "phone.h"
#include "wifi.h"

#define CONSOLE_UART   UART_NUM_0
#define CONSOLE_LINE_MAX       1100
#define PROMPT_LINE    300   /* base64 한 줄 길이 (4의 배수) */

static const settings_t *s_running;
static char *s_prompt_buf;   /* cfg prompt begin ~ end 사이에 모으는 지시문 */
static size_t s_prompt_len;
static b64_decoder_t s_prompt_b64;

static void reply(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void reply(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    printf("cfg ");
    vprintf(fmt, args);
    printf("\n");
    fflush(stdout);
    va_end(args);
}

static void show(void)
{
    const settings_t *s = s_running;
    size_t key_len = strlen(s->api_key);
    reply("value wifi_ssid=%s", s->wifi_ssid);
    reply("value wifi_pass=%s", s->wifi_pass[0] ? "(saved)" : "");
    if (key_len > 8) {
        reply("value api_key=%.4s...%s", s->api_key, s->api_key + key_len - 4);
    } else {
        reply("value api_key=%s", key_len ? "(saved)" : "");
    }
    reply("value model=%s", s->model);
    reply("value rings=%d", s->rings);
    reply("value prompt=%s %u", s->prompt_custom ? "custom" : "default", (unsigned)strlen(s->prompt));
    reply("end show");
}

static void prompt_get(void)
{
    const uint8_t *p = (const uint8_t *)s_running->prompt;
    size_t len = strlen(s_running->prompt);
    const size_t raw_per_line = PROMPT_LINE / 4 * 3;
    char line[PROMPT_LINE + 1];
    for (size_t off = 0; off < len; off += raw_per_line) {
        size_t n = len - off < raw_per_line ? len - off : raw_per_line;
        line[b64_encode(p + off, n, line)] = '\0';
        reply("prompt data %s", line);
    }
    reply("prompt end");
}

static void prompt_command(const char *arg)
{
    if (strcmp(arg, "get") == 0) {
        prompt_get();
    } else if (strcmp(arg, "begin") == 0) {
        free(s_prompt_buf);
        s_prompt_buf = malloc(SETTINGS_PROMPT_MAX);
        s_prompt_len = 0;
        b64_decoder_reset(&s_prompt_b64);
        if (s_prompt_buf) {
            reply("ok prompt begin");
        } else {
            reply("err prompt no-memory");
        }
    } else if (strncmp(arg, "data ", 5) == 0) {
        if (s_prompt_buf == NULL) {
            reply("err prompt not-begun");
            return;
        }
        for (const char *c = arg + 5; *c; c++) {
            uint8_t byte;
            if (b64_decode_char(&s_prompt_b64, *c, &byte)) {
                if (s_prompt_len >= SETTINGS_PROMPT_MAX) {
                    free(s_prompt_buf);
                    s_prompt_buf = NULL;
                    reply("err prompt too-long");
                    return;
                }
                s_prompt_buf[s_prompt_len++] = (char)byte;
            }
        }
    } else if (strcmp(arg, "end") == 0) {
        if (s_prompt_buf == NULL) {
            reply("err prompt not-begun");
            return;
        }
        esp_err_t err = settings_set_prompt(s_prompt_buf, s_prompt_len);
        free(s_prompt_buf);
        s_prompt_buf = NULL;
        if (err == ESP_OK) {
            reply("ok prompt %u", (unsigned)s_prompt_len);
        } else {
            reply("err prompt %s", esp_err_to_name(err));
        }
    } else if (strcmp(arg, "reset") == 0) {
        esp_err_t err = settings_set_prompt(NULL, 0);
        if (err == ESP_OK) {
            reply("ok prompt reset");
        } else {
            reply("err prompt %s", esp_err_to_name(err));
        }
    } else {
        reply("err prompt unknown");
    }
}

static void handle_line(char *line)
{
    if (strncmp(line, "cfg set ", 8) == 0) {
        char *key = line + 8;
        char *value = strchr(key, ' ');
        if (value == NULL) {
            value = "";
        } else {
            *value++ = '\0';
        }
        esp_err_t err = settings_set(key, value);
        if (err == ESP_OK) {
            reply("ok %s", key);
        } else {
            reply("err %s %s", key, esp_err_to_name(err));
        }
    } else if (strcmp(line, "cfg show") == 0) {
        show();
    } else if (strncmp(line, "cfg prompt ", 11) == 0) {
        prompt_command(line + 11);
    } else if (strcmp(line, "cfg reboot") == 0) {
        reply("ok reboot");
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    } else if (strcmp(line, "pair") == 0) {
        phone_pair();
        reply("ok pair");
    } else if (strcmp(line, "status") == 0) {
        phone_status_t st;
        phone_get_status(&st);
        reply("status phone=%s peer=%s audio=%d pairing=%d wifi=%d heap=%u min_heap=%u",
              phone_conn_state_name(st.conn_state), st.peer, st.audio, st.pairing, wifi_connected(),
              (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    } else if (line[0] != '\0') {
        reply("err unknown-command");
    }
}

static void console_task(void *arg)
{
    static char line[CONSOLE_LINE_MAX];
    size_t len = 0;
    for (;;) {
        uint8_t c;
        if (uart_read_bytes(CONSOLE_UART, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\n') {
            line[len] = '\0';
            handle_line(line);
            len = 0;
        } else if (c != '\r' && len < CONSOLE_LINE_MAX - 1) {
            line[len++] = (char)c;
        }
    }
}

void console_start(const settings_t *running)
{
    s_running = running;
    ESP_ERROR_CHECK(uart_driver_install(CONSOLE_UART, 2048, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(CONSOLE_UART);
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
}
