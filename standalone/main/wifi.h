/* Wi-Fi 연결 (끊기면 자동 재연결) */
#pragma once

#include <stdbool.h>

void wifi_start(const char *ssid, const char *password);
bool wifi_connected(void);
int wifi_rssi(void);  /* 공유기 신호 세기 (dBm, 연결 안 됐으면 0) */
