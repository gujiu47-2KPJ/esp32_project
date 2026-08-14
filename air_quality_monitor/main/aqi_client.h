#ifndef AQI_CLIENT_H
#define AQI_CLIENT_H

#include "main.h"

void aqi_client_init(void);
esp_err_t fetch_outdoor_aqi(outdoor_aqi_data_t *data);

#endif