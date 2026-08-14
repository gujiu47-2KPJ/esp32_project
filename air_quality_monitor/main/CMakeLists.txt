idf_component_register(
    SRCS "main.c" "wifi_manager.c" "aqi_client.c" "uart_comm.c" "aqi_processor.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_http_client esp_wifi nvs_flash esp_event cjson
)