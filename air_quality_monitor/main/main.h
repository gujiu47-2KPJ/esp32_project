#ifndef MAIN_H
#define MAIN_H

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"

#define WIFI_CONNECTED_BIT BIT0
#define AQI_FETCHED_BIT    BIT1
#define DATA_PROCESSED_BIT BIT2

typedef struct {
    float aqi;
    float pm25;
    float pm10;
    float co;
    float so2;
    float no2;
    float o3;
    char level[32];
    char suggestion[128];
} outdoor_aqi_data_t;

typedef struct {
    float mq135_ppm;
    float temperature;
    float humidity;
    float calculated_aqi;
    char level[32];
} indoor_aqi_data_t;

typedef struct {
    outdoor_aqi_data_t outdoor;
    indoor_aqi_data_t indoor;
    char comparison_result[256];
    char final_suggestion[256];
} comprehensive_aqi_data_t;

extern EventGroupHandle_t aqi_event_group;

#endif