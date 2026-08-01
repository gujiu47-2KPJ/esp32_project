idf_component_register(SRCS "hello_world_main.c"
                       PRIV_REQUIRES nvs_flash esp_driver_uart esp_driver_gpio esp_wifi esp_http_client esp_http_server esp_event espressif__cjson
                       INCLUDE_DIRS "")