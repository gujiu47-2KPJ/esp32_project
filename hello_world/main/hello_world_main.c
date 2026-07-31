/*
 * ESP32-S3 Air Quality Analysis System
 * 
 * 功能：
 * 1. 通过UART接收STM32传输的MQ-135传感器数据
 * 2. 通过WiFi获取室外空气质量报告
 * 3. 分析室内外空气质量差异
 * 4. 生成健康评估和改进建议
 * 5. 将分析结果发送回STM32用于OLED显示
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "cJSON.h"

/* ==================== 配置参数 ==================== */

/* WiFi配置 */
#define WIFI_SSID      "YOUR_WIFI_SSID"
#define WIFI_PASSWORD  "YOUR_WIFI_PASSWORD"
#define WIFI_MAX_RETRY  5

/* UART配置 (与STM32通信) */
#define STM_UART_NUM        UART_NUM_1
#define STM_UART_TX_PIN     GPIO_NUM_4
#define STM_UART_RX_PIN     GPIO_NUM_5
#define STM_UART_BAUD_RATE  115200
#define UART_BUF_SIZE       1024

/* 室外空气质量API配置 (OpenWeatherMap) */
#define WEATHER_API_KEY    "YOUR_OWM_API_KEY"
#define WEATHER_API_URL    "http://api.openweathermap.org/data/2.5/air_pollution?lat=22.7459&lon=114.1366&appid=" WEATHER_API_KEY
/* 位置：广东东莞凤岗 (22.7459, 114.1366) */

/* 数据采集间隔 (毫秒) */
#define DATA_SAMPLE_INTERVAL_MS  10000  /* 10秒 */

/* ==================== 日志标签 ==================== */
static const char *TAG = "AIR_QUALITY";

/* ==================== 数据结构定义 ==================== */

/* STM32传感器数据 */
typedef struct {
    float co2_ppm;      /* CO2浓度 (PPM) */
    float temp;         /* 温度 */
    float humi;         /* 湿度 */
    float aqi;          /* AQI指数 */
    char level[20];     /* 空气质量等级 */
} stm_sensor_data_t;

/* 室外空气质量数据 */
typedef struct {
    int aqi;            /* AQI指数 (0-500) */
    float co;           /* CO浓度 (μg/m³) */
    float no2;          /* NO2浓度 (μg/m³) */
    float o3;           /* O3浓度 (μg/m³) */
    float pm2_5;        /* PM2.5浓度 (μg/m³) */
    float pm10;         /* PM10浓度 (μg/m³) */
    float so2;          /* SO2浓度 (μg/m³) */
} outdoor_air_data_t;

/* 分析结果 */
typedef struct {
    int indoor_aqi;         /* 室内AQI */
    int outdoor_aqi;        /* 室外AQI */
    int aqi_difference;     /* AQI差异 */
    float ventilation_score;/* 通风建议评分 (0-100) */
    int health_level;       /* 健康等级 (1-6) */
    char suggestion[256];   /* 改进建议 */
    char health_advice[128];/* 健康评估 */
} analysis_result_t;

/* ==================== 全局变量 ==================== */

static EventGroupHandle_t wifi_event_group;
const int WIFI_CONNECTED_BIT = BIT0;
static int wifi_retry = 0;
static bool outdoor_data_ready = false;
static outdoor_air_data_t outdoor_data;
static stm_sensor_data_t indoor_data;

/* ==================== WiFi连接函数 ==================== */

static void event_handler(void* arg, esp_event_base_t event_base,
                          int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (wifi_retry < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            wifi_retry++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        wifi_retry = 0;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    /* 等待WiFi连接 */
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s", WIFI_SSID);
    } else {
        ESP_LOGW(TAG, "WiFi connection failed, will work in offline mode");
    }
}

/* ==================== HTTP客户端获取室外空气质量 ==================== */

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    static char *output_buffer;
    static int output_len;

    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (output_buffer == NULL) {
                output_buffer = (char *) malloc(512);
                output_len = 0;
                if (output_buffer == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
                    return ESP_FAIL;
                }
            }
            
            if (output_len + evt->data_len < 512) {
                memcpy(output_buffer + output_len, evt->data, evt->data_len);
                output_len += evt->data_len;
            }
            break;
            
        case HTTP_EVENT_ON_FINISH:
            if (output_buffer != NULL) {
                /* 解析JSON响应 */
                cJSON *root = cJSON_Parse(output_buffer);
                if (root != NULL) {
                    cJSON *list = cJSON_GetObjectItem(root, "list");
                    if (list != NULL && cJSON_GetArraySize(list) > 0) {
                        cJSON *item = cJSON_GetArrayItem(list, 0);
                        cJSON *main = cJSON_GetObjectItem(item, "main");
                        cJSON *components = cJSON_GetObjectItem(item, "components");
                        
                        if (main != NULL) {
                            cJSON *aqi_item = cJSON_GetObjectItem(main, "aqi");
                            if (aqi_item != NULL) {
                                outdoor_data.aqi = aqi_item->valueint;
                            }
                        }
                        
                        if (components != NULL) {
                            cJSON *co_item = cJSON_GetObjectItem(components, "co");
                            if (co_item != NULL) { outdoor_data.co = co_item->valuedouble; }
                            cJSON *no2_item = cJSON_GetObjectItem(components, "no2");
                            if (no2_item != NULL) { outdoor_data.no2 = no2_item->valuedouble; }
                            cJSON *o3_item = cJSON_GetObjectItem(components, "o3");
                            if (o3_item != NULL) { outdoor_data.o3 = o3_item->valuedouble; }
                            cJSON *pm25_item = cJSON_GetObjectItem(components, "pm2_5");
                            if (pm25_item != NULL) { outdoor_data.pm2_5 = pm25_item->valuedouble; }
                            cJSON *pm10_item = cJSON_GetObjectItem(components, "pm10");
                            if (pm10_item != NULL) { outdoor_data.pm10 = pm10_item->valuedouble; }
                            cJSON *so2_item = cJSON_GetObjectItem(components, "so2");
                            if (so2_item != NULL) { outdoor_data.so2 = so2_item->valuedouble; }
                        }
                        
                        outdoor_data_ready = true;
                        ESP_LOGI(TAG, "Outdoor AQI: %d", outdoor_data.aqi);
                    }
                    cJSON_Delete(root);
                }
                free(output_buffer);
                output_buffer = NULL;
                output_len = 0;
            }
            break;
            
        default:
            break;
    }
    return ESP_OK;
}

static void fetch_outdoor_air_quality(void)
{
    esp_http_client_config_t config = {
        .url = WEATHER_API_URL,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HTTP GET request completed successfully");
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    
    esp_http_client_cleanup(client);
}

/* ==================== UART通信函数 ==================== */

static void uart_init(void)
{
    uart_config_t uart_config = {
        .baud_rate = STM_UART_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    
    ESP_ERROR_CHECK(uart_driver_install(STM_UART_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(STM_UART_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(STM_UART_NUM, STM_UART_TX_PIN, STM_UART_RX_PIN, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    
    ESP_LOGI(TAG, "UART initialized on GPIO%d(TX), GPIO%d(RX)", STM_UART_TX_PIN, STM_UART_RX_PIN);
}

static bool parse_stm32_data(const char *data, stm_sensor_data_t *parsed_data)
{
    /* 解析格式：MQ135:PPM=450.00,TEMP=25.00,HUMI=50.00,AQI=450.00,LEVEL=Good\r\n */
    
    char *ppm_str = strstr(data, "PPM=");
    char *temp_str = strstr(data, "TEMP=");
    char *humi_str = strstr(data, "HUMI=");
    char *aqi_str = strstr(data, "AQI=");
    char *level_str = strstr(data, "LEVEL=");
    
    if (ppm_str && temp_str && humi_str && aqi_str && level_str) {
        parsed_data->co2_ppm = atof(ppm_str + 4);
        parsed_data->temp = atof(temp_str + 5);
        parsed_data->humi = atof(humi_str + 5);
        parsed_data->aqi = atof(aqi_str + 4);
        
        /* 提取LEVEL字符串 */
        char *end = strstr(level_str + 6, "\r");
        if (end == NULL) {
            end = strstr(level_str + 6, "\n");
        }
        
        int level_len = (end != NULL) ? (end - (level_str + 6)) : strlen(level_str + 6);
        strncpy(parsed_data->level, level_str + 6, level_len);
        parsed_data->level[level_len] = '\0';
        
        return true;
    }
    
    return false;
}

static void send_to_stm32(const char *message)
{
    uart_write_bytes(STM_UART_NUM, message, strlen(message));
    uart_write_bytes(STM_UART_NUM, "\n", 1);
    ESP_LOGI(TAG, "Sent to STM32: %s", message);
}

/* ==================== 空气质量分析算法 ==================== */

static int calculate_indoor_aqi(float co2_ppm)
{
    /* 基于CO2浓度计算室内AQI (简化模型) */
    /* CO2 < 600: 优秀, 600-1000: 良好, 1000-2000: 一般, >2000: 差 */
    
    if (co2_ppm < 400) return (int)(co2_ppm / 8);
    else if (co2_ppm < 1000) return 50 + (int)((co2_ppm - 400) / 12);
    else if (co2_ppm < 2000) return 100 + (int)((co2_ppm - 1000) / 10);
    else return 200 + (int)((co2_ppm - 2000) / 10);
}

static void analyze_air_quality(analysis_result_t *result)
{
    /* 计算室内AQI */
    result->indoor_aqi = calculate_indoor_aqi(indoor_data.co2_ppm);
    
    /* 获取室外AQI */
    if (outdoor_data_ready) {
        result->outdoor_aqi = outdoor_data.aqi;
    } else {
        result->outdoor_aqi = 100; /* 默认值 */
    }
    
    /* 计算差异 */
    result->aqi_difference = result->indoor_aqi - result->outdoor_aqi;
    
    /* 通风建议评分 */
    if (result->aqi_difference > 50) {
        /* 室内远差于室外，强烈建议通风 */
        result->ventilation_score = 90;
    } else if (result->aqi_difference > 20) {
        /* 室内稍差于室外，建议通风 */
        result->ventilation_score = 70;
    } else if (result->aqi_difference > -20) {
        /* 室内外相近，可适度通风 */
        result->ventilation_score = 50;
    } else if (result->aqi_difference > -50) {
        /* 室外稍差，谨慎通风 */
        result->ventilation_score = 30;
    } else {
        /* 室外远差于室内，不建议通风 */
        result->ventilation_score = 10;
    }
    
    /* 健康等级评估 (1-6) */
    if (result->indoor_aqi <= 50) {
        result->health_level = 1;
        strcpy(result->health_advice, "Excellent");
    } else if (result->indoor_aqi <= 100) {
        result->health_level = 2;
        strcpy(result->health_advice, "Good");
    } else if (result->indoor_aqi <= 150) {
        result->health_level = 3;
        strcpy(result->health_advice, "Moderate");
    } else if (result->indoor_aqi <= 200) {
        result->health_level = 4;
        strcpy(result->health_advice, "Poor");
    } else if (result->indoor_aqi <= 300) {
        result->health_level = 5;
        strcpy(result->health_advice, "Bad");
    } else {
        result->health_level = 6;
        strcpy(result->health_advice, "Hazardous");
    }
    
    /* 生成改进建议：综合室内 AQI 与室内外差异（通风评分） */
    if (result->indoor_aqi > 200) {
        /* 室内很差：无论室外如何都要处理 */
        sprintf(result->suggestion, "Open windows! Use air purifier!");
    } else if (result->ventilation_score >= 70) {
        /* 室内明显差于室外：强烈建议通风 */
        sprintf(result->suggestion, "Ventilate room now! Open windows!");
    } else if (result->ventilation_score <= 30) {
        /* 室外明显差于室内：不建议通风，保持关闭 */
        sprintf(result->suggestion, "Keep windows closed!");
    } else if (result->indoor_aqi > 100) {
        /* 室内一般，可适度通风 */
        sprintf(result->suggestion, "Good time to ventilate room");
    } else if (result->indoor_aqi > 50) {
        sprintf(result->suggestion, "Air quality is acceptable");
    } else {
        sprintf(result->suggestion, "Excellent air quality!");
    }
}

/* ==================== 主任务函数 ==================== */

static void stm32_communication_task(void *pvParameters)
{
    char rx_buffer[256];
    char line_buffer[256];
    int  line_len = 0;
    
    ESP_LOGI(TAG, "STM32 communication task started");
    
    while (1) {
        int len = uart_read_bytes(STM_UART_NUM, rx_buffer, sizeof(rx_buffer) - 1, 100 / portTICK_PERIOD_MS);
        
        if (len > 0) {
            /* 逐字节累积成行，遇 \r\n 才算一条完整数据，避免半帧解析失败 */
            for (int i = 0; i < len; i++) {
                char c = rx_buffer[i];
                
                if (c == '\n' || c == '\r') {
                    if (line_len > 0) {
                        line_buffer[line_len] = '\0';
                        line_len = 0;
                        
                        ESP_LOGI(TAG, "Received from STM32: %s", line_buffer);
                        
                        /* 解析STM32数据 */
                        if (parse_stm32_data(line_buffer, &indoor_data)) {
                            ESP_LOGI(TAG, "Parsed - CO2: %.2f PPM, AQI: %.2f, Level: %s",
                                    indoor_data.co2_ppm, indoor_data.aqi, indoor_data.level);
                            
                            /* 执行分析 */
                            analysis_result_t result;
                            analyze_air_quality(&result);
                            
                            /* 发送分析结果到STM32 */
                            send_to_stm32(result.suggestion);
                            
                            /* 打印详细分析结果到控制台 */
                            ESP_LOGI(TAG, "=== Air Quality Analysis ===");
                            ESP_LOGI(TAG, "Indoor AQI: %d", result.indoor_aqi);
                            ESP_LOGI(TAG, "Outdoor AQI: %d", result.outdoor_aqi);
                            ESP_LOGI(TAG, "Difference: %d", result.aqi_difference);
                            ESP_LOGI(TAG, "Ventilation Score: %.0f%%", result.ventilation_score);
                            ESP_LOGI(TAG, "Health Level: %d - %s", result.health_level, result.health_advice);
                            ESP_LOGI(TAG, "Suggestion: %s", result.suggestion);
                            ESP_LOGI(TAG, "===========================");
                        }
                    }
                } else if (line_len < (int)sizeof(line_buffer) - 1) {
                    line_buffer[line_len++] = c;
                }
            }
        }
        
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

static void outdoor_data_fetch_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Outdoor data fetch task started");
    
    /* 在此任务内初始化并连接WiFi：即使WiFi连不上，
       也不影响 stm32_communication_task 响应STM32数据 */
    ESP_LOGI(TAG, "Connecting to WiFi...");
    wifi_init_sta();
    
    /* 等待WiFi连接稳定 */
    vTaskDelay(5000 / portTICK_PERIOD_MS);
    
    while (1) {
        if (outdoor_data_ready) {
            ESP_LOGI(TAG, "Outdoor AQI: %d (PM2.5: %.1f, PM10: %.1f)",
                    outdoor_data.aqi, outdoor_data.pm2_5, outdoor_data.pm10);
        } else {
            ESP_LOGW(TAG, "Fetching outdoor air quality data...");
            fetch_outdoor_air_quality();
        }
        
        /* 每10分钟更新一次室外数据 */
        vTaskDelay(600000 / portTICK_PERIOD_MS);
    }
}

/* ==================== 主函数 ==================== */

void app_main(void)
{
    ESP_LOGI(TAG, "ESP32-S3 Air Quality Analysis System Starting...");
    
    /* 初始化NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    /* 初始化UART */
    uart_init();
    
    /* 先创建任务，WiFi 连接已在 outdoor_data_fetch_task 内进行，
       避免 WiFi 连不上时阻塞对 STM32 的响应 */
    xTaskCreate(stm32_communication_task, "stm32_comm", 4096, NULL, 5, NULL);
    xTaskCreate(outdoor_data_fetch_task, "outdoor_fetch", 8192, NULL, 4, NULL);
    
    ESP_LOGI(TAG, "System ready. Waiting for STM32 data...");
    
    /* 发送就绪信号到STM32 */
    send_to_stm32("ESP32 Ready");
}