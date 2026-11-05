#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"

typedef struct esp_websocket_client *esp_websocket_client_handle_t;

typedef enum {
    WEBSOCKET_EVENT_ANY = -1,
    WEBSOCKET_EVENT_ERROR = 0,
    WEBSOCKET_EVENT_CONNECTED,
    WEBSOCKET_EVENT_DISCONNECTED,
    WEBSOCKET_EVENT_DATA,
    WEBSOCKET_EVENT_CLOSED,
} esp_websocket_event_id_t;

enum {
    WS_TRANSPORT_OPCODES_CONT = 0x00,
    WS_TRANSPORT_OPCODES_TEXT = 0x01,
    WS_TRANSPORT_OPCODES_BINARY = 0x02,
    WS_TRANSPORT_OPCODES_CLOSE = 0x08,
};

typedef struct {
    int esp_ws_handshake_status_code;
} esp_websocket_error_codes_t;

typedef struct {
    const char *data_ptr;
    int data_len;
    bool fin;
    uint8_t op_code;
    esp_websocket_client_handle_t client;
    void *user_context;
    int payload_len;
    int payload_offset;
    esp_websocket_error_codes_t error_handle;
} esp_websocket_event_data_t;

typedef struct {
    const char *uri;
    const char *headers;
    int buffer_size;
    int task_stack;
    int task_prio;
    const char *cert_pem;
    bool disable_auto_reconnect;
    int network_timeout_ms;
    bool keep_alive_enable;
} esp_websocket_client_config_t;

typedef void (*esp_event_handler_t)(void *arg, esp_event_base_t base, int32_t id, void *data);

esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t c, esp_websocket_event_id_t id,
                                        esp_event_handler_t handler, void *arg);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t c);
esp_err_t esp_websocket_client_close(esp_websocket_client_handle_t c, TickType_t timeout);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t c);
bool esp_websocket_client_is_connected(esp_websocket_client_handle_t c);
int esp_websocket_client_send_text(esp_websocket_client_handle_t c, const char *data, int len, TickType_t timeout);
