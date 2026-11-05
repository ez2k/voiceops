idf_component_register(SRCS "main.c" "agent.c" "console.c" "settings.c" "wifi.c"
                            "realtime.c" "rt_stream.c" "b64.c" "g711.c" "pcm_ring.c"
                       INCLUDE_DIRS "."
                       EMBED_TXTFILES "prompt.md" "google_roots.pem"
                       PRIV_REQUIRES phone nvs_flash esp_wifi esp_netif esp_event esp_driver_uart esp_app_format esp_coex
                                     espressif__esp_websocket_client espressif__cjson)
