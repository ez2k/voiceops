idf_component_register(SRCS "main.c" "agent.c" "console.c" "settings.c" "wifi.c"
                            "gemini.c" "gemini_stream.c" "b64.c" "decimator.c" "pcm_ring.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "prompt.md"
                       PRIV_REQUIRES phone nvs_flash esp_wifi esp_netif esp_event esp_driver_uart mbedtls
                                     espressif__esp_websocket_client espressif__cjson)
