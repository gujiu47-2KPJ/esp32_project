#ifndef AQI_PROCESSOR_H
#define AQI_PROCESSOR_H

#include "main.h"

void aqi_processor_init(void);
float calculate_indoor_aqi(float mq135_ppm, float temperature, float humidity);
void generate_suggestion(comprehensive_aqi_data_t *data);
void aqi_processor_task(void *pvParameters);

#endif