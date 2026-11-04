idf_component_register(SRCS "main.c" "frame.c" "link.c"
                       PRIV_REQUIRES phone nvs_flash esp_driver_uart
                       INCLUDE_DIRS ".")
