/*
 * ESP32-S3 室内外空气质量监测系统
 * 
 * 功能：
 * 1. 通过 UART1 接收 STM32 室内空气质量数据
 * 2. 通过 WiFi 连接网络，调用 OpenWeatherMap API 获取室外空气质量
 * 3. 对比室内外空气质量，生成建议
 * 4. 发送室外数据和建议给 STM32
 * 
 * 通信协议：
 * - 接收：MQ135:ADC=xxx,V=x.xxx,RS=xx.xx,RSR=x.xxx,CO2=xx.xx,CO=xx.xx,ALC=xx.xx,TOL=xx.xx,NH4=xx.xx,ACE=xx.xx,AQ=x\r\n
 * - 发送室外数据：OUTDOOR:CO2=xx.xx,CO=xx.xx,ALC=xx.xx,TOL=xx.xx,NH4=xx.xx,ACE=xx.xx\r\n
 * - 发送建议：ADVICE:xxxxxxxxxxxxxxxx\r\n
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "nvs_flash.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "cJSON.h"

/* ==================== 配置参数 ==================== */

/* UART配置 */
#define STM_UART_NUM        UART_NUM_1
#define STM_UART_TX_PIN     GPIO_NUM_14   /* TX: GPIO14 → STM32 PA10 (原 GPIO17 接触不良已换) */
#define STM_UART_RX_PIN     GPIO_NUM_15   /* RX: GPIO15 ← STM32 PA9 (原 GPIO16 接触不良已换) */
#define STM_UART_BAUD_RATE  115200
#define UART_BUF_SIZE       2048

/* WiFi配置 */
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"

/* OpenWeatherMap API配置 */
#define OWM_API_KEY         "YOUR_OWM_API_KEY"  /* 替换为你的API Key */
#define OWM_LAT             22.75f                /* 纬度（东莞凤岗） */
#define OWM_LON             114.15f               /* 经度（东莞凤岗） */
#define OWM_UPDATE_INTERVAL (5 * 60 * 1000)       /* 5分钟更新一次 */

/* 事件组 */
#define WIFI_CONNECTED_BIT  BIT0
#define API_DATA_READY_BIT  BIT1

/* 日志标签 */
static const char *TAG = "AIR_MONITOR";

/* ==================== 数据结构定义 ==================== */

/* 室内空气质量数据（来自STM32） */
typedef struct {
    float co2;
    float co;
    float alcohol;
    float toluene;
    float nh3;
    float acetone;
    int   aq_level;
    float temperature;  /* 室内温度 (℃) - AHT20/BMP280 */
    float humidity;     /* 室内湿度 (%RH) - AHT20 */
    float pressure;     /* 室内气压 (hPa) - BMP280 */
    bool  valid;
    time_t last_update;
} indoor_data_t;

/* 室外空气质量数据（来自API） */
typedef struct {
    float co2;
    float co;
    float aqi;
    int   aqi_level;
    bool  valid;
    time_t last_update;
} outdoor_data_t;

/* ==================== 全局变量 ==================== */

static EventGroupHandle_t s_wifi_event_group;
static indoor_data_t indoor_data = {0};
static outdoor_data_t outdoor_data = {0};
static TickType_t last_outdoor_update = 0;

/* ==================== WiFi 连接 ==================== */

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init_sta(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
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

    ESP_LOGI(TAG, "WiFi initialized. SSID: %s", WIFI_SSID);
}

static bool wifi_wait_connected(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                           WIFI_CONNECTED_BIT,
                                           pdFALSE,
                                           pdFALSE,
                                           pdMS_TO_TICKS(15000));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

/* ==================== HTTP 客户端 ==================== */

/* HTTP响应缓冲区 */
static char http_response_buffer[2048];
static int http_response_len = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (http_response_len + evt->data_len < sizeof(http_response_buffer)) {
                memcpy(http_response_buffer + http_response_len, evt->data, evt->data_len);
                http_response_len += evt->data_len;
                http_response_buffer[http_response_len] = '\0';
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

/**
 * @brief 获取室外空气质量数据
 */
static bool fetch_outdoor_air_quality(void)
{
    char url[256];
    snprintf(url, sizeof(url),
             "http://api.openweathermap.org/data/2.5/air_pollution?lat=%.4f&lon=%.4f&appid=%s",
             OWM_LAT, OWM_LON, OWM_API_KEY);

    ESP_LOGI(TAG, "Requesting URL: %s", url);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .disable_auto_redirect = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    http_response_len = 0;
    memset(http_response_buffer, 0, sizeof(http_response_buffer));

    ESP_LOGI(TAG, "Performing HTTP request...");
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return false;
    }

    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP status code: %d", status_code);
    ESP_LOGI(TAG, "Response: %s", http_response_buffer);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "HTTP status code: %d", status_code);
        esp_http_client_cleanup(client);
        return false;
    }

    /* 解析JSON响应 */
    cJSON *root = cJSON_Parse(http_response_buffer);
    if (root == NULL) {
        ESP_LOGE(TAG, "JSON parse failed");
        esp_http_client_cleanup(client);
        return false;
    }

    cJSON *list = cJSON_GetObjectItem(root, "list");
    if (list && cJSON_GetArraySize(list) > 0) {
        cJSON *item = cJSON_GetArrayItem(list, 0);
        cJSON *components = cJSON_GetObjectItem(item, "components");
        cJSON *main_obj = cJSON_GetObjectItem(item, "main");

        if (components) {
            /* 获取CO浓度（μg/m³），转换为PPM */
            cJSON *co_json = cJSON_GetObjectItem(components, "co");
            if (co_json) {
                outdoor_data.co = (co_json->valuedouble / 1000.0f) / 1.145f;
                ESP_LOGI(TAG, "CO: %.2f μg/m³ = %.2f PPM", co_json->valuedouble, outdoor_data.co);
            }

            /* 获取其他气体 */
            cJSON *no2_json = cJSON_GetObjectItem(components, "no2");
            cJSON *o3_json = cJSON_GetObjectItem(components, "o3");
            cJSON *so2_json = cJSON_GetObjectItem(components, "so2");
            cJSON *pm25_json = cJSON_GetObjectItem(components, "pm2_5");
            cJSON *pm10_json = cJSON_GetObjectItem(components, "pm10");

            /* 估算CO2（API不直接提供，根据AQI估算） */
            if (main_obj) {
                cJSON *aqi_json = cJSON_GetObjectItem(main_obj, "aqi");
                if (aqi_json) {
                    outdoor_data.aqi = (float)aqi_json->valueint;
                    /* AQI 1-5 映射到 CO2 估算值 */
                    outdoor_data.co2 = 400.0f + (outdoor_data.aqi - 1) * 50.0f;
                    outdoor_data.aqi_level = aqi_json->valueint;
                    ESP_LOGI(TAG, "AQI: %d, Estimated CO2: %.1f PPM", outdoor_data.aqi_level, outdoor_data.co2);
                }
            }

            outdoor_data.valid = true;
    outdoor_data.last_update = time(NULL);
    ESP_LOGI(TAG, "Outdoor AQ: CO2=%.1f PPM, CO=%.2f PPM, AQI=%.0f (Level %d)",
                     outdoor_data.co2, outdoor_data.co, outdoor_data.aqi, outdoor_data.aqi_level);
        }
    } else {
        ESP_LOGE(TAG, "No data in response");
    }

    cJSON_Delete(root);
    esp_http_client_cleanup(client);
    return outdoor_data.valid;
}

/**
 * @brief 使用模拟室外数据（测试用）
 */
static void use_mock_outdoor_data(void)
{
    outdoor_data.co2 = 420.0f;   /* 清洁空气约400-450 PPM */
    outdoor_data.co = 2.0f;      /* 约2 PPM */
    outdoor_data.aqi = 1.0f;     /* 优秀 */
    outdoor_data.aqi_level = 1;
    outdoor_data.valid = true;
    outdoor_data.last_update = time(NULL);
    
    ESP_LOGI(TAG, "Using mock outdoor data: CO2=%.1f, CO=%.1f, AQI=1", outdoor_data.co2, outdoor_data.co);
}

/* ==================== UART通信 ==================== */

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
    
    ESP_LOGI(TAG, "UART1 initialized: TX=GPIO%d, RX=GPIO%d, Baud=%d", 
             STM_UART_TX_PIN, STM_UART_RX_PIN, STM_UART_BAUD_RATE);
}

/**
 * @brief 解析STM32数据
 */
static bool parse_stm32_data(const char *data, indoor_data_t *parsed)
{
    if (strstr(data, "MQ135:") == NULL) {
        return false;
    }

    char *token;
    char *saveptr;
    char data_copy[512];
    
    strncpy(data_copy, data, sizeof(data_copy) - 1);
    data_copy[sizeof(data_copy) - 1] = '\0';
    
    char *data_start = data_copy + 6;
    
    token = strtok_r(data_start, ",", &saveptr);
    while (token != NULL) {
        if (strncmp(token, "CO2=", 4) == 0) {
            parsed->co2 = atof(token + 4);
        } else if (strncmp(token, "CO=", 3) == 0) {
            parsed->co = atof(token + 3);
        } else if (strncmp(token, "ALC=", 4) == 0) {
            parsed->alcohol = atof(token + 4);
        } else if (strncmp(token, "TOL=", 4) == 0) {
            parsed->toluene = atof(token + 4);
        } else if (strncmp(token, "NH4=", 4) == 0) {
            parsed->nh3 = atof(token + 4);
        } else if (strncmp(token, "ACE=", 4) == 0) {
            parsed->acetone = atof(token + 4);
        } else if (strncmp(token, "AQ=", 3) == 0) {
            parsed->aq_level = atoi(token + 3);
        } else if (strncmp(token, "TEMP=", 5) == 0) {
            parsed->temperature = atof(token + 5);
        } else if (strncmp(token, "HUMI=", 5) == 0) {
            parsed->humidity = atof(token + 5);
        } else if (strncmp(token, "PRES=", 5) == 0) {
            parsed->pressure = atof(token + 5);
        }
        
        token = strtok_r(NULL, ",", &saveptr);
    }
    
    parsed->valid = true;
    parsed->last_update = time(NULL);
    return true;
}

/**
 * @brief 发送室外数据给STM32
 */
static void send_outdoor_data(void)
{
    if (!outdoor_data.valid) return;
    
    char tx_buffer[256];
    int len = snprintf(tx_buffer, sizeof(tx_buffer),
        "OUTDOOR:CO2=%.2f,CO=%.2f,ALC=%.2f,TOL=%.2f,NH4=%.2f,ACE=%.2f\r\n",
        outdoor_data.co2, outdoor_data.co, 1.0f, 1.0f, 1.0f, 1.0f);
    
    uart_write_bytes(STM_UART_NUM, tx_buffer, len);
    ESP_LOGI(TAG, "Sent outdoor data: %s", tx_buffer);
}

/**
 * @brief 发送建议给STM32
 */
static void send_advice(const char *advice)
{
    char tx_buffer[256];
    int len = snprintf(tx_buffer, sizeof(tx_buffer), "ADVICE:%s\r\n", advice);
    uart_write_bytes(STM_UART_NUM, tx_buffer, len);
    ESP_LOGI(TAG, "Sent advice: %s", tx_buffer);
}

/**
 * @brief 发送融合数据给STM32
 * @note   FUSION:CO2=...,CO=...,ALC=...,TOL=...,NH4=...,ACE=...,AQ=x
 *         融合值以室内实测为主，AQ 等级按室内 CO2 分级（与 STM32 一致）
 */
static void send_fusion_data(void)
{
    if (!indoor_data.valid) return;
    
    /* 融合 AQ 等级：<600优秀 / 600-1000良好 / 1000-1500一般 / 1500-2000较差 / 2000-3000差 / >3000危险 */
    int fused_aq;
    if (indoor_data.co2 < 600.0f) fused_aq = 0;
    else if (indoor_data.co2 < 1000.0f) fused_aq = 1;
    else if (indoor_data.co2 < 1500.0f) fused_aq = 2;
    else if (indoor_data.co2 < 2000.0f) fused_aq = 3;
    else if (indoor_data.co2 < 3000.0f) fused_aq = 4;
    else fused_aq = 5;
    
    char tx_buffer[256];
    int len = snprintf(tx_buffer, sizeof(tx_buffer),
        "FUSION:CO2=%.2f,CO=%.2f,ALC=%.2f,TOL=%.2f,NH4=%.2f,ACE=%.2f,AQ=%d\r\n",
        indoor_data.co2, indoor_data.co, indoor_data.alcohol, indoor_data.toluene,
        indoor_data.nh3, indoor_data.acetone, fused_aq);
    
    uart_write_bytes(STM_UART_NUM, tx_buffer, len);
    ESP_LOGI(TAG, "Sent fusion: %s", tx_buffer);
}

/* ==================== 室内外对比分析 ==================== */

/**
 * @brief 对比室内外空气质量，生成建议
 */
static void compare_and_advise(void)
{
    if (!indoor_data.valid || !outdoor_data.valid) return;
    
    float co2_diff = indoor_data.co2 - outdoor_data.co2;
    float co_diff = indoor_data.co - outdoor_data.co;
    
    char advice[256];
    
    if (co2_diff > 200.0f) {
        /* 室内CO2比室外高200PPM以上 */
        snprintf(advice, sizeof(advice),
                 "CO2 high! Indoor %.0f vs Outdoor %.0f. Ventilate!",
                 indoor_data.co2, outdoor_data.co2);
    } else if (indoor_data.co2 > 1000.0f) {
        snprintf(advice, sizeof(advice),
                 "Ventilate room! CO2 %.0f PPM", indoor_data.co2);
    } else if (indoor_data.humidity > 75.0f) {
        /* 湿度过高：提示通风除湿 */
        snprintf(advice, sizeof(advice),
                 "Humid %.0f%%! Ventilate to reduce", indoor_data.humidity);
    } else if (indoor_data.temperature > 32.0f) {
        /* 温度过高：提示通风降温 */
        snprintf(advice, sizeof(advice),
                 "Hot %.1fC! Ventilate", indoor_data.temperature);
    } else if (co_diff > 20.0f) {
        snprintf(advice, sizeof(advice),
                 "CO high! Check gas source");
    } else if (co2_diff < -100.0f) {
        snprintf(advice, sizeof(advice),
                 "Outdoor worse than indoor. Keep windows closed.");
    } else {
        snprintf(advice, sizeof(advice),
                 "Air quality OK. Indoor %.0f vs Outdoor %.0f.",
                 indoor_data.co2, outdoor_data.co2);
    }
    
    send_advice(advice);
    vTaskDelay(pdMS_TO_TICKS(20));   /* 间隔发送，避免 STM32 接收缓冲被后续消息覆盖 */
    send_fusion_data();
}

/* ==================== Web 服务器 ==================== */

/* HTML 页面 */
static const char *index_html = 
"<!DOCTYPE html>\n"
"<html lang=\"zh-CN\">\n"
"<head>\n"
"    <meta charset=\"UTF-8\">\n"
"    <meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n"
"    <title>空气质量监测系统</title>\n"
"    <style>\n"
"        * { margin: 0; padding: 0; box-sizing: border-box; }\n"
"        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); min-height: 100vh; padding: 20px; }\n"
"        .container { max-width: 1200px; margin: 0 auto; }\n"
"        h1 { color: white; text-align: center; margin-bottom: 30px; text-shadow: 2px 2px 4px rgba(0,0,0,0.3); }\n"
"        .grid { display: grid; grid-template-columns: repeat(auto-fit, minmax(350px, 1fr)); gap: 20px; margin-bottom: 20px; }\n"
"        .card { background: white; border-radius: 15px; padding: 25px; box-shadow: 0 10px 30px rgba(0,0,0,0.2); }\n"
"        .card h2 { color: #333; margin-bottom: 15px; padding-bottom: 10px; border-bottom: 2px solid #667eea; }\n"
"        .data-row { display: flex; justify-content: space-between; padding: 10px 0; border-bottom: 1px solid #eee; }\n"
"        .data-label { color: #666; font-weight: 500; }\n"
"        .data-value { color: #333; font-weight: bold; font-size: 1.1em; }\n"
"        .status { padding: 10px 15px; border-radius: 8px; margin-top: 15px; text-align: center; font-weight: bold; }\n"
"        .status.good { background: #d4edda; color: #155724; }\n"
"        .status.moderate { background: #fff3cd; color: #856404; }\n"
"        .status.bad { background: #f8d7da; color: #721c24; }\n"
"        .status.waiting { background: #e2e3e5; color: #383d41; }\n"
"        .api-section { background: white; border-radius: 15px; padding: 25px; box-shadow: 0 10px 30px rgba(0,0,0,0.2); }\n"
"        .api-section h2 { color: #333; margin-bottom: 15px; }\n"
"        .api-item { background: #f8f9fa; padding: 15px; border-radius: 8px; margin-bottom: 10px; }\n"
"        .api-url { font-family: monospace; background: #e9ecef; padding: 8px 12px; border-radius: 5px; display: block; margin: 8px 0; word-break: break-all; }\n"
"        .api-desc { color: #666; font-size: 0.9em; }\n"
"        .refresh-btn { background: #667eea; color: white; border: none; padding: 12px 25px; border-radius: 8px; cursor: pointer; font-size: 1em; margin-top: 15px; }\n"
"        .refresh-btn:hover { background: #5a6fd6; }\n"
"        .timestamp { color: #999; font-size: 0.85em; margin-top: 10px; }\n"
"    </style>\n"
"</head>\n"
"<body>\n"
"    <div class=\"container\">\n"
"        <h1>室内外空气质量监测系统</h1>\n"
"        \n"
"        <div class=\"grid\">\n"
"            <div class=\"card\">\n"
"                <h2>室内空气质量 (STM32)</h2>\n"
"                <div id=\"indoor-data\">\n"
"                    <div class=\"data-row\"><span class=\"data-label\">CO2</span><span class=\"data-value\" id=\"indoor-co2\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">温度</span><span class=\"data-value\" id=\"indoor-temp\">-- ℃</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">湿度</span><span class=\"data-value\" id=\"indoor-humi\">-- %</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">气压</span><span class=\"data-value\" id=\"indoor-pres\">-- hPa</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">CO</span><span class=\"data-value\" id=\"indoor-co\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">酒精</span><span class=\"data-value\" id=\"indoor-alcohol\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">甲苯</span><span class=\"data-value\" id=\"indoor-toluene\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">氨气</span><span class=\"data-value\" id=\"indoor-nh3\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">丙酮</span><span class=\"data-value\" id=\"indoor-acetone\">-- PPM</span></div>\n"
"                </div>\n"
"                <div class=\"status waiting\" id=\"indoor-status\">等待数据...</div>\n"
"                <div class=\"timestamp\" id=\"indoor-time\"></div>\n"
"            </div>\n"
"            \n"
"            <div class=\"card\">\n"
"                <h2>室外空气质量 (ESP32)</h2>\n"
"                <div id=\"outdoor-data\">\n"
"                    <div class=\"data-row\"><span class=\"data-label\">CO2 (估算)</span><span class=\"data-value\" id=\"outdoor-co2\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">CO</span><span class=\"data-value\" id=\"outdoor-co\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">AQI</span><span class=\"data-value\" id=\"outdoor-aqi\">--</span></div>\n"
"                </div>\n"
"                <div class=\"status waiting\" id=\"outdoor-status\">等待数据...</div>\n"
"                <div class=\"timestamp\" id=\"outdoor-time\"></div>\n"
"            </div>\n"
"            \n"
"            <div class=\"card\">\n"
"                <h2>室内外对比分析</h2>\n"
"                <div id=\"compare-data\">\n"
"                    <div class=\"data-row\"><span class=\"data-label\">CO2 差值</span><span class=\"data-value\" id=\"co2-diff\">-- PPM</span></div>\n"
"                    <div class=\"data-row\"><span class=\"data-label\">CO 差值</span><span class=\"data-value\" id=\"co-diff\">-- PPM</span></div>\n"
"                </div>\n"
"                <div class=\"status waiting\" id=\"compare-status\">等待数据...</div>\n"
"                <button class=\"refresh-btn\" onclick=\"refreshData()\">刷新数据</button>\n"
"            </div>\n"
"        </div>\n"
"        \n"
"        <div class=\"api-section\">\n"
"            <h2>REST API 接口</h2>\n"
"            <div class=\"api-item\">\n"
"                <span class=\"api-desc\">获取室内数据：</span>\n"
"                <code class=\"api-url\">curl http://&lt;ESP32_IP&gt;/api/indoor</code>\n"
"            </div>\n"
"            <div class=\"api-item\">\n"
"                <span class=\"api-desc\">获取室外数据：</span>\n"
"                <code class=\"api-url\">curl http://&lt;ESP32_IP&gt;/api/outdoor</code>\n"
"            </div>\n"
"            <div class=\"api-item\">\n"
"                <span class=\"api-desc\">获取对比分析：</span>\n"
"                <code class=\"api-url\">curl http://&lt;ESP32_IP&gt;/api/compare</code>\n"
"            </div>\n"
"            <div class=\"api-item\">\n"
"                <span class=\"api-desc\">获取所有数据：</span>\n"
"                <code class=\"api-url\">curl http://&lt;ESP32_IP&gt;/api/all</code>\n"
"            </div>\n"
"        </div>\n"
"    </div>\n"
"    \n"
"    <script>\n"
"        function updateStatus(elementId, status, text) {\n"
"            const el = document.getElementById(elementId);\n"
"            el.className = 'status ' + status;\n"
"            el.textContent = text;\n"
"        }\n"
"        \n"
"        function formatTime(timestamp) {\n"
"            if (!timestamp) return '无数据';\n"
"            const date = new Date(timestamp * 1000);\n"
"            return '更新时间: ' + date.toLocaleString('zh-CN');\n"
"        }\n"
"        \n"
"        async function refreshData() {\n"
"            try {\n"
"                const indoorRes = await fetch('/api/indoor');\n"
"                const indoor = await indoorRes.json();\n"
"                \n"
"                if (indoor.valid) {\n"
"                    document.getElementById('indoor-co2').textContent = indoor.co2.toFixed(1) + ' PPM';\n"
"                    document.getElementById('indoor-temp').textContent = indoor.temperature.toFixed(1) + ' ℃';\n"
"                    document.getElementById('indoor-humi').textContent = indoor.humidity.toFixed(1) + ' %';\n"
"                    document.getElementById('indoor-pres').textContent = indoor.pressure.toFixed(1) + ' hPa';\n"
"                    document.getElementById('indoor-co').textContent = indoor.co.toFixed(1) + ' PPM';\n"
"                    document.getElementById('indoor-alcohol').textContent = indoor.alcohol.toFixed(1) + ' PPM';\n"
"                    document.getElementById('indoor-toluene').textContent = indoor.toluene.toFixed(1) + ' PPM';\n"
"                    document.getElementById('indoor-nh3').textContent = indoor.nh3.toFixed(1) + ' PPM';\n"
"                    document.getElementById('indoor-acetone').textContent = indoor.acetone.toFixed(1) + ' PPM';\n"
"                    \n"
"                    const levels = ['Excellent', 'Good', 'Moderate', 'Poor', 'Bad', 'Hazardous'];\n"
"                    updateStatus('indoor-status', indoor.co2 < 600 ? 'good' : indoor.co2 < 1000 ? 'moderate' : 'bad',\n"
"                        '等级: ' + levels[indoor.aq_level] + ' (' + indoor.aq_level + ')');\n"
"                    document.getElementById('indoor-time').textContent = formatTime(indoor.timestamp);\n"
"                } else {\n"
"                    updateStatus('indoor-status', 'waiting', '等待数据...');\n"
"                }\n"
"                \n"
"                const outdoorRes = await fetch('/api/outdoor');\n"
"                const outdoor = await outdoorRes.json();\n"
"                \n"
"                if (outdoor.valid) {\n"
"                    document.getElementById('outdoor-co2').textContent = outdoor.co2.toFixed(1) + ' PPM';\n"
"                    document.getElementById('outdoor-co').textContent = outdoor.co.toFixed(2) + ' PPM';\n"
"                    document.getElementById('outdoor-aqi').textContent = outdoor.aqi + ' (等级 ' + outdoor.aqi_level + ')';\n"
"                    updateStatus('outdoor-status', outdoor.aqi <= 2 ? 'good' : outdoor.aqi <= 4 ? 'moderate' : 'bad',\n"
"                        'AQI: ' + outdoor.aqi);\n"
"                    document.getElementById('outdoor-time').textContent = formatTime(outdoor.timestamp);\n"
"                } else {\n"
"                    updateStatus('outdoor-status', 'waiting', '等待数据...');\n"
"                }\n"
"                \n"
"                const compareRes = await fetch('/api/compare');\n"
"                const compare = await compareRes.json();\n"
"                \n"
"                if (compare.valid) {\n"
"                    document.getElementById('co2-diff').textContent = compare.co2_diff.toFixed(1) + ' PPM';\n"
"                    document.getElementById('co-diff').textContent = compare.co_diff.toFixed(2) + ' PPM';\n"
"                    updateStatus('compare-status', compare.co2_diff > 200 ? 'bad' : compare.co2_diff > 100 ? 'moderate' : 'good',\n"
"                        compare.advice);\n"
"                } else {\n"
"                    updateStatus('compare-status', 'waiting', '等待数据...');\n"
"                }\n"
"            } catch (error) {\n"
"                console.error('Error fetching data:', error);\n"
"            }\n"
"        }\n"
"        \n"
"        refreshData();\n"
"        setInterval(refreshData, 10000);\n"
"    </script>\n"
"</body>\n"
"</html>\n";

/* Web 服务器处理函数 */

static esp_err_t index_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, index_html, strlen(index_html));
    return ESP_OK;
}

static esp_err_t api_indoor_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "co2", indoor_data.co2);
    cJSON_AddNumberToObject(root, "co", indoor_data.co);
    cJSON_AddNumberToObject(root, "alcohol", indoor_data.alcohol);
    cJSON_AddNumberToObject(root, "toluene", indoor_data.toluene);
    cJSON_AddNumberToObject(root, "nh3", indoor_data.nh3);
    cJSON_AddNumberToObject(root, "acetone", indoor_data.acetone);
    cJSON_AddNumberToObject(root, "aq_level", indoor_data.aq_level);
    cJSON_AddNumberToObject(root, "temperature", indoor_data.temperature);
    cJSON_AddNumberToObject(root, "humidity", indoor_data.humidity);
    cJSON_AddNumberToObject(root, "pressure", indoor_data.pressure);
    cJSON_AddBoolToObject(root, "valid", indoor_data.valid);
    cJSON_AddNumberToObject(root, "timestamp", (double)indoor_data.last_update);
    
    char *json_str = cJSON_PrintUnformatted(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_outdoor_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "co2", outdoor_data.co2);
    cJSON_AddNumberToObject(root, "co", outdoor_data.co);
    cJSON_AddNumberToObject(root, "aqi", outdoor_data.aqi);
    cJSON_AddNumberToObject(root, "aqi_level", outdoor_data.aqi_level);
    cJSON_AddBoolToObject(root, "valid", outdoor_data.valid);
    cJSON_AddNumberToObject(root, "timestamp", (double)outdoor_data.last_update);
    
    char *json_str = cJSON_PrintUnformatted(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_compare_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
    if (indoor_data.valid && outdoor_data.valid) {
        float co2_diff = indoor_data.co2 - outdoor_data.co2;
        float co_diff = indoor_data.co - outdoor_data.co;
        
        cJSON_AddNumberToObject(root, "co2_diff", co2_diff);
        cJSON_AddNumberToObject(root, "co_diff", co_diff);
        cJSON_AddBoolToObject(root, "valid", true);
        
        char advice[128];
        if (co2_diff > 200.0f) {
            snprintf(advice, sizeof(advice), "CO2 high! Indoor %.0f vs Outdoor %.0f. Ventilate!",
                     indoor_data.co2, outdoor_data.co2);
        } else if (co2_diff > 100.0f) {
            snprintf(advice, sizeof(advice), "CO2 moderate. Indoor %.0f vs Outdoor %.0f.",
                     indoor_data.co2, outdoor_data.co2);
        } else {
            snprintf(advice, sizeof(advice), "Air quality OK. Indoor %.0f vs Outdoor %.0f.",
                     indoor_data.co2, outdoor_data.co2);
        }
        cJSON_AddStringToObject(root, "advice", advice);
    } else {
        cJSON_AddBoolToObject(root, "valid", false);
        cJSON_AddStringToObject(root, "advice", "No data for comparison");
    }
    
    char *json_str = cJSON_PrintUnformatted(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t api_all_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    
    cJSON *indoor = cJSON_CreateObject();
    cJSON_AddNumberToObject(indoor, "co2", indoor_data.co2);
    cJSON_AddNumberToObject(indoor, "co", indoor_data.co);
    cJSON_AddNumberToObject(indoor, "alcohol", indoor_data.alcohol);
    cJSON_AddNumberToObject(indoor, "toluene", indoor_data.toluene);
    cJSON_AddNumberToObject(indoor, "nh3", indoor_data.nh3);
    cJSON_AddNumberToObject(indoor, "acetone", indoor_data.acetone);
    cJSON_AddNumberToObject(indoor, "aq_level", indoor_data.aq_level);
    cJSON_AddNumberToObject(indoor, "temperature", indoor_data.temperature);
    cJSON_AddNumberToObject(indoor, "humidity", indoor_data.humidity);
    cJSON_AddNumberToObject(indoor, "pressure", indoor_data.pressure);
    cJSON_AddBoolToObject(indoor, "valid", indoor_data.valid);
    cJSON_AddItemToObject(root, "indoor", indoor);
    
    cJSON *outdoor = cJSON_CreateObject();
    cJSON_AddNumberToObject(outdoor, "co2", outdoor_data.co2);
    cJSON_AddNumberToObject(outdoor, "co", outdoor_data.co);
    cJSON_AddNumberToObject(outdoor, "aqi", outdoor_data.aqi);
    cJSON_AddNumberToObject(outdoor, "aqi_level", outdoor_data.aqi_level);
    cJSON_AddBoolToObject(outdoor, "valid", outdoor_data.valid);
    cJSON_AddItemToObject(root, "outdoor", outdoor);
    
    char *json_str = cJSON_PrintUnformatted(root);
    
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    
    free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

static void start_web_server(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 10;
    
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_index = {
            .uri      = "/",
            .method   = HTTP_GET,
            .handler  = index_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_index);
        
        httpd_uri_t uri_indoor = {
            .uri      = "/api/indoor",
            .method   = HTTP_GET,
            .handler  = api_indoor_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_indoor);
        
        httpd_uri_t uri_outdoor = {
            .uri      = "/api/outdoor",
            .method   = HTTP_GET,
            .handler  = api_outdoor_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_outdoor);
        
        httpd_uri_t uri_compare = {
            .uri      = "/api/compare",
            .method   = HTTP_GET,
            .handler  = api_compare_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_compare);
        
        httpd_uri_t uri_all = {
            .uri      = "/api/all",
            .method   = HTTP_GET,
            .handler  = api_all_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(server, &uri_all);
        
        ESP_LOGI(TAG, "Web server started on port %d", config.server_port);
    }
}

/* ==================== 室外数据更新任务 ==================== */

static void outdoor_update_task(void *pvParameters)
{
    ESP_LOGI(TAG, "Outdoor data update task started");
    
    /* 等待WiFi连接 */
    if (!wifi_wait_connected()) {
        ESP_LOGW(TAG, "WiFi not connected, using mock data");
        use_mock_outdoor_data();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    
    while (1) {
        TickType_t now = xTaskGetTickCount();
        
        /* 首次启动立即获取一次，之后每5分钟更新一次 */
        if (last_outdoor_update == 0 ||
            now - last_outdoor_update > pdMS_TO_TICKS(OWM_UPDATE_INTERVAL)) {
            ESP_LOGI(TAG, "Fetching outdoor air quality data...");
            
            if (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) {
                if (!fetch_outdoor_air_quality()) {
                    ESP_LOGW(TAG, "Failed to fetch outdoor data, using mock");
                    use_mock_outdoor_data();
                }
            } else {
                ESP_LOGW(TAG, "WiFi disconnected, using mock data");
                use_mock_outdoor_data();
            }
            
            last_outdoor_update = now;
            
            /* 发送室外数据给STM32 */
            if (outdoor_data.valid) {
                send_outdoor_data();
                
                /* 如果室内数据也有效，进行对比 */
                if (indoor_data.valid) {
                    compare_and_advise();
                }
            }
        }
        
        vTaskDelay(1000 / portTICK_PERIOD_MS);
    }
}

/* ==================== 主处理任务 ==================== */

static void mq135_processing_task(void *pvParameters)
{
    uint8_t rx_buffer[UART_BUF_SIZE];
    char line_buffer[512];
    int line_len = 0;
    
    ESP_LOGI(TAG, "MQ-135 processing task started");
    
    while (1) {
        int len = uart_read_bytes(STM_UART_NUM, rx_buffer, sizeof(rx_buffer) - 1, 100 / portTICK_PERIOD_MS);
        
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                char c = rx_buffer[i];
                
                if (c == '\n' || c == '\r') {
                    if (line_len > 0) {
                        line_buffer[line_len] = '\0';
                        line_len = 0;
                        
                        ESP_LOGI(TAG, "Received: %s", line_buffer);
                        
                        /* 解析室内数据 */
                        indoor_data_t new_data = {0};
                        if (parse_stm32_data(line_buffer, &new_data)) {
                            indoor_data = new_data;
                            
                            ESP_LOGI(TAG, "Indoor: CO2=%.1f, CO=%.1f, ALC=%.1f, TOL=%.1f, NH4=%.1f, ACE=%.1f, AQ=%d",
                                     indoor_data.co2, indoor_data.co, indoor_data.alcohol,
                                     indoor_data.toluene, indoor_data.nh3, indoor_data.acetone,
                                     indoor_data.aq_level);
                            
                            /* 如果室外数据有效，进行对比 */
                            if (outdoor_data.valid) {
                                send_outdoor_data();
                                vTaskDelay(pdMS_TO_TICKS(20));   /* 间隔发送，避免 STM32 接收缓冲被后续消息覆盖 */
                                compare_and_advise();
                            }
                        }
                    }
                } else if (line_len < (int)sizeof(line_buffer) - 1) {
                    line_buffer[line_len++] = c;
                } else {
                    ESP_LOGW(TAG, "Buffer overflow, resetting");
                    line_len = 0;
                }
            }
        }
        
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

/* ==================== 主函数 ==================== */

void app_main(void)
{
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "ESP32-S3 Indoor/Outdoor Air Quality Monitor");
    ESP_LOGI(TAG, "=============================================");
    ESP_LOGI(TAG, "WiFi SSID: %s", WIFI_SSID);
    ESP_LOGI(TAG, "API Key: %s", OWM_API_KEY);
    ESP_LOGI(TAG, "Location: lat=%.2f, lon=%.2f", OWM_LAT, OWM_LON);
    ESP_LOGI(TAG, "=============================================");
    
    /* 初始化NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    /* 初始化UART */
    uart_init();
    
    /* 初始化WiFi */
    wifi_init_sta();
    
    /* 启动Web服务器 */
    start_web_server();
    
    /* 创建任务 */
    xTaskCreate(mq135_processing_task, "mq135_proc", 8192, NULL, 5, NULL);
    xTaskCreate(outdoor_update_task, "outdoor_upd", 8192, NULL, 4, NULL);
    
    ESP_LOGI(TAG, "System ready. Waiting for STM32 data...");
    
    const char *ready_msg = "ESP32 Air Monitor Ready\n";
    uart_write_bytes(STM_UART_NUM, ready_msg, strlen(ready_msg));
}