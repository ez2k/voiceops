/* Wi-Fi 연결 (끊기면 자동 재연결) */
#pragma once

#include <stdbool.h>

void wifi_start(const char *ssid, const char *password);
bool wifi_connected(void);
