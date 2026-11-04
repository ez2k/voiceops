idf_component_register(SRCS "phone.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_common
                       PRIV_REQUIRES bt nvs_flash esp_timer)
