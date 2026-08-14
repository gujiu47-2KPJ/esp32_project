#include "main.h"
#include "uart_comm.h"
#include "driver/uart.h"
#include "driver/gpio.h"

static const char *TAG = "UART_COMM";

#define UART_TX_PIN     GPIO_NUM_17
#define UART_RX_PIN     GPIO_NUM_16
#define UART_NUM        UART_NUM_1

#define UART_BAUD_RATE  115200
#define UART_BUF_SIZE   (1024)

typedef struct {
    uint8_t header[2];
    uint8_t length;
    uint8_t type;
    uint8_t payload[128];
    uint8_t checksum;
    uint8_t footer[2];
} uart_frame_t;

void uart_comm_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM, 
                                         UART_BUF_SIZE,
                                         UART_BUF_SIZE,
                                         0,
                                         NULL, 
                                         0));
    
    ESP_ERROR_CHECK(uart_param_config(UART_NUM, &uart_config));
    
    ESP_ERROR_CHECK(uart_set_pin(UART_NUM, 
                                  UART_TX_PIN,
                                  UART_RX_PIN,
                                  UART_PIN_NO_CHANGE,
                                  UART_PIN_NO_CHANGE));
    
    ESP_LOGI(TAG, "UART%d initialized: TX=GPIO%d, RX=GPIO%d, Baud=%d", 
             UART_NUM, UART_TX_PIN, UART_RX_PIN, UART_BAUD_RATE);
}

void uart_comm_task(void *pvParameters)
{
    indoor_aqi_data_t indoor_data;
    uint8_t rx_data[512];
    
    uart_comm_init();
    
    ESP_LOGI(TAG, "UART communication task started");
    
    while(1) {
        /* 等待 AQI 获取完成，然后从 STM32 接收数据 */
        EventBits_t bits = xEventGroupWaitBits(
            aqi_event_group,
            AQI_FETCHED_BIT,
            pdFALSE,  /* 不清除标志位 */
            pdFALSE,  /* 任意一个标志位即可 */
            pdMS_TO_TICKS(100)
        );
        
        /* 尝试从 STM32 读取数据 */
        int len = uart_read_bytes(UART_NUM, rx_data, sizeof(rx_data) - 1, 
                                   pdMS_TO_TICKS(50));
        
        if (len > 0) {
            rx_data[len] = '\0';
            ESP_LOGI(TAG, "Received %d bytes from STM32: %s", len, (char*)rx_data);
        }
        
        /* 如果数据处理完成，发送建议到 STM32 */
        if (xEventGroupGetBits(aqi_event_group) & DATA_PROCESSED_BIT) {
            ESP_LOGI(TAG, "Data processed bit set, ready to send suggestions");
        }
        
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

esp_err_t send_to_stm32(const char *data, size_t len)
{
    /* 添加换行符作为消息结束标记 */
    char tx_buffer[512];
    size_t tx_len = len;
    
    if (tx_len >= sizeof(tx_buffer) - 2) {
        tx_len = sizeof(tx_buffer) - 3;
    }
    
    memcpy(tx_buffer, data, tx_len);
    tx_buffer[tx_len] = '\r';
    tx_buffer[tx_len + 1] = '\n';
    tx_buffer[tx_len + 2] = '\0';
    
    int tx_bytes = uart_write_bytes(UART_NUM, tx_buffer, tx_len + 2);
    if (tx_bytes < 0) {
        ESP_LOGE(TAG, "Failed to send data to STM32");
        return ESP_FAIL;
    }
    
    ESP_LOGI(TAG, "Sent suggestion to STM32 (%d bytes)", tx_bytes);
    return ESP_OK;
}

esp_err_t receive_from_stm32(indoor_aqi_data_t *data)
{
    uint8_t rx_buffer[512];
    int len = uart_read_bytes(UART_NUM, rx_buffer, sizeof(rx_buffer) - 1, 
                               pdMS_TO_TICKS(2000));
    
    if (len <= 0) {
        return ESP_FAIL;
    }
    
    rx_buffer[len] = '\0';
    ESP_LOGI(TAG, "Received from STM32: %s", (char*)rx_buffer);
    
    /* 解析 STM32 发送的数据格式：MQ135:PPM=%.2f,TEMP=%.2f,HUMI=%.2f,AQI=%.2f,LEVEL=%s */
    if (strncmp((char*)rx_buffer, "MQ135:", 6) == 0) {
        float ppm = 0, temp = 0, humi = 0, aqi = 0;
        char level[32] = {0};
        
        /* 解析各个字段 */
        char *ppm_ptr = strstr((char*)rx_buffer, "PPM=");
        char *temp_ptr = strstr((char*)rx_buffer, "TEMP=");
        char *humi_ptr = strstr((char*)rx_buffer, "HUMI=");
        char *aqi_ptr = strstr((char*)rx_buffer, "AQI=");
        char *level_ptr = strstr((char*)rx_buffer, "LEVEL=");
        
        if (ppm_ptr) {
            sscanf(ppm_ptr, "PPM=%f", &ppm);
        }
        if (temp_ptr) {
            sscanf(temp_ptr, "TEMP=%f", &temp);
        }
        if (humi_ptr) {
            sscanf(humi_ptr, "HUMI=%f", &humi);
        }
        if (aqi_ptr) {
            sscanf(aqi_ptr, "AQI=%f", &aqi);
        }
        if (level_ptr) {
            /* 提取等级字符串，去掉结尾的 \r\n */
            sscanf(level_ptr, "LEVEL=%31[^\r\n]", level);
        }
        
        data->mq135_ppm = ppm;
        data->temperature = temp;
        data->humidity = humi;
        data->calculated_aqi = aqi;
        strncpy(data->level, level, sizeof(data->level) - 1);
        data->level[sizeof(data->level) - 1] = '\0';
        
        ESP_LOGI(TAG, "Parsed - PPM: %.2f, Temp: %.2f, Humi: %.2f, AQI: %.2f, Level: %s",
                 ppm, temp, humi, aqi, level);
        
        return ESP_OK;
    }
    
    return ESP_FAIL;
}