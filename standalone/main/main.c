/*
 * AI CallBridge — 단독형 펌웨어 (ESP32-WROOM-32 하나로)
 *
 *   휴대폰 ⇄ 블루투스 HFP ⇄ ESP32 ⇄ Wi-Fi ⇄ Gemini Live
 *
 * 설정(Wi-Fi, API 키, 지시문 등)은 웹 설정 페이지나 시리얼 "cfg" 명령으로 넣고 재부팅하면 적용된다.
 */
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "agent.h"
#include "console.h"
#include "settings.h"
#include "wifi.h"

#define TAG                 "main"
#define CALLBRIDGE_VERSION  "0.1.0"

static settings_t s_settings;

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    settings_load(&s_settings);
    console_start(&s_settings);
    agent_start(&s_settings);

    if (s_settings.wifi_ssid[0]) {
        wifi_start(s_settings.wifi_ssid, s_settings.wifi_pass);
    } else {
        ESP_LOGW(TAG, "Wi-Fi 설정이 없습니다 — 웹 설정 페이지에서 입력하세요");
    }
    if (!s_settings.api_key[0]) {
        ESP_LOGW(TAG, "Gemini API 키가 없습니다 — 웹 설정 페이지에서 입력하세요");
    }

    ESP_LOGI(TAG, "heap after start: free=%u min=%u",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    printf("CALLBRIDGE READY %s\n", CALLBRIDGE_VERSION);
    fflush(stdout);
}
