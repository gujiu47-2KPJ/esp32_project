#ifndef UART_COMM_H
#define UART_COMM_H

#include "main.h"

void uart_comm_init(void);
esp_err_t send_to_stm32(const char *data, size_t len);
esp_err_t receive_from_stm32(indoor_aqi_data_t *data);

#endif