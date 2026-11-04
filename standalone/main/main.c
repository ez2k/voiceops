/*
 * AI CallBridge — 단독형 펌웨어 (ESP32-WROOM-32 하나로)
 *
 *   휴대폰 ⇄ 블루투스 HFP ⇄ ESP32 ⇄ Wi-Fi ⇄ Gemini Live
 *
 * 설정(Wi-Fi, API 키, 지시문 등)은 웹 설정 페이지나 시리얼 "cfg" 명령으로 넣고 재부팅하면 적용된다.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_coexist.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "agent.h"
#include "console.h"
#include "settings.h"
#include "wifi.h"

#define TAG "main"

static settings_t s_settings;

/* 블루투스 통화와 Wi-Fi 가 무선(안테나 하나)을 나눠 쓰는 비율. 통화 중 AI 음성(약 64KB/s)이 모자라면 wifi 로.
 * IDF 가 deprecated 로 표시한 API 라 칩·버전에 따라 효과가 없을 수 있다 — 통화 통계의 rx 로 확인한다. */
static void apply_coex(const char *coex)
{
    esp_coex_prefer_t prefer = strcmp(coex, "wifi") == 0 ? ESP_COEX_PREFER_WIFI
                             : strcmp(coex, "bt") == 0   ? ESP_COEX_PREFER_BT
                                                         : ESP_COEX_PREFER_BALANCE;
    esp_err_t err = esp_coex_preference_set(prefer);
    ESP_LOGI(TAG, "무선 우선: %s (%s)", coex, esp_err_to_name(err));
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    settings_load(&s_settings);
    agent_start(&s_settings);
    console_start(&s_settings);  /* pair·status 명령이 블루투스를 쓰므로 그 뒤에 */

    if (s_settings.wifi_ssid[0]) {
        wifi_start(s_settings.wifi_ssid, s_settings.wifi_pass);
        apply_coex(s_settings.coex);
    } else {
        ESP_LOGW(TAG, "Wi-Fi 설정이 없습니다 — 웹 설정 페이지에서 입력하세요");
    }
    if (!s_settings.api_key[0]) {
        ESP_LOGW(TAG, "Gemini API 키가 없습니다 — 웹 설정 페이지에서 입력하세요");
    }

    ESP_LOGI(TAG, "heap after start: free=%u min=%u",
             (unsigned)esp_get_free_heap_size(), (unsigned)esp_get_minimum_free_heap_size());
    printf("CALLBRIDGE READY %s\n", esp_app_get_description()->version);
    fflush(stdout);
}
