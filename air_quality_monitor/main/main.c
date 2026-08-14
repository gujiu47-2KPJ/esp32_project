#include "main.h"

extern void wifi_manager_task(void *pvParameters);
extern void aqi_client_task(void *pvParameters);
extern void uart_comm_task(void *pvParameters);
extern void aqi_processor_task(void *pvParameters);

EventGroupHandle_t aqi_event_group;

void app_main(void)
{
    ESP_LOGI("MAIN", "ESP32 Air Quality Monitor System Starting...");
    
    aqi_event_group = xEventGroupCreate();
    
    xTaskCreate(wifi_manager_task, "wifi_manager", 4096, NULL, 5, NULL);
    xTaskCreate(aqi_client_task, "aqi_client", 8192, NULL, 3, NULL);
    xTaskCreate(uart_comm_task, "uart_comm", 4096, NULL, 4, NULL);
    xTaskCreate(aqi_processor_task, "aqi_processor", 4096, NULL, 3, NULL);
    
    ESP_LOGI("MAIN", "All tasks created successfully");
    
    vTaskDelete(NULL);
}