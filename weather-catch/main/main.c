#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "driver/gpio.h"
#include "driver/uart.h"

#include "esp_http_server.h"
#include "esp_http_client.h"
#include "cJSON.h"
#include "lwip/err.h"
#include "lwip/sys.h"

/* ============================================================
 *    用户配置区（根据实际情况修改以下参数
 * ============================================================ */

/* 1. WiFi 名称和密码（改成你自己的WiFi） */
#define WIFI_SSID           "YOUR_WIFI_SSID"
#define WIFI_PASSWORD       "YOUR_WIFI_PASSWORD"

/* 2. 和风天气 API（推荐，免费版足够用
 * 官网注册：https://dev.qweather.com/
 * 东莞市凤岗镇  location: 101281609 （可以改成你幼儿园附近的站点
 * 如果不想注册API，也可以用下面"电脑推送模式"跳过这一步 */
#define QWEATHER_KEY         "YOUR_QWEATHER_API_KEY"
#define QWEATHER_LOCATION    "101281609"

/* 3. 板载 LED 引脚 */
#define BLINK_GPIO           GPIO_NUM_2

/* 4. UART2 与 STM32 通讯引脚
 *    ESP32 TX(GPIO17) -> STM32 RX
 *    ESP32 RX(GPIO16) <- STM32 TX
 *    GND 必须共地！ */
#define UART_STM32_NUM       UART_NUM_2
#define UART_STM32_TX_PIN    GPIO_NUM_17
#define UART_STM32_RX_PIN    GPIO_NUM_16
#define UART_STM32_BAUD    9600
#define UART_BUF_SIZE       1024

/* 5. HTTP Server 端口（电脑访问用） */
#define HTTP_SERVER_PORT     80

/* ============================================================ */

static const char *TAG = "WEATHER_CATCH";

/* 全局天气数据（线程安全访问 */
typedef struct {
    float temperature;
    float humidity;
    bool  valid;         /* 数据是否有效 */
    uint32_t update_ts;  /* 更新时间戳 */
    SemaphoreHandle_t lock;
} weather_data_t;

static weather_data_t g_weather = {
    .temperature = 0.0f,
    .humidity = 0.0f,
    .valid = false,
    .update_ts = 0,
    .lock = NULL
};

/* WiFi 连接事件组 */
static EventGroupHandle_t s_wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1
static int s_retry_num = 0;
#define MAXIMUM_RETRY     10

/* ============================================================
 *  UART2 -> STM32 通讯（自定义协议，STM32端请按此解析：
 *  帧格式：| 0xAA | 0x55 | TEMP_H | TEMP_L | HUM_H | HUM_L | CHK | 0x0D | 0x0A
 *  说明：
 *    - 帧头：0xAA 0x55
 *    - TEMP：温度 x100 （int16，小端），例 25.50°C -> 2550 = 0x09F6 -> TEMP_H=0x09 TEMP_L=0xF6
 *    - HUM： 湿度 x100 （int16，小端），例 65.20% -> 6520 = 0x1978
 *    - CHK： 前面6字节的异或校验
 *    - 帧尾：0x0D 0x0A (\r\n)
 * ============================================================ */
static void uart_send_to_stm32(float temp, float hum)
{
    int16_t temp_int = (int16_t)(temp * 100.0f);
    int16_t hum_int  = (int16_t)(hum  * 100.0f);

    uint8_t frame[9];
    frame[0] = 0xAA;
    frame[1] = 0x55;
    frame[2] = (uint8_t)(temp_int >> 8);
    frame[3] = (uint8_t)(temp_int & 0xFF);
    frame[4] = (uint8_t)(hum_int >> 8);
    frame[5] = (uint8_t)(hum_int & 0xFF);
    /* 校验：frame[0]^frame[1]^frame[2]^frame[3]^frame[4]^frame[5] */
    frame[6] = frame[0]^frame[1]^frame[2]^frame[3]^frame[4]^frame[5];
    frame[7] = 0x0D;
    frame[8] = 0x0A;

    uart_write_bytes(UART_STM32_NUM, frame, sizeof(frame));
    ESP_LOGI(TAG, "UART->STM32: T=%.2fC H=%.2f%% (frame sent", temp, hum);
}

/* ============================================================
 *  WiFi 驱动
 * ============================================================ */
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retry_num < MAXIMUM_RETRY) {
            esp_wifi_connect();
            s_retry_num++;
            ESP_LOGI(TAG, "retry to connect to the AP");
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
        ESP_LOGI(TAG,"connect to the AP fail");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
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
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {
                .capable = true,
                .required = false
            },
        },
    };
    memcpy(wifi_config.sta.ssid, WIFI_SSID, strlen(WIFI_SSID));
    memcpy(wifi_config.sta.password, WIFI_PASSWORD, strlen(WIFI_PASSWORD));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA) );
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config) );
    ESP_ERROR_CHECK(esp_wifi_start() );

    ESP_LOGI(TAG, "wifi_init_sta finished.");

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
            pdFALSE,
            pdFALSE,
            portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to ap SSID:%s password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s",
                 WIFI_SSID, WIFI_PASSWORD);
    } else {
        ESP_LOGE(TAG, "UNEXPECTED EVENT");
    }
}

/* ============================================================
 *  HTTP Server：电脑浏览器或脚本通过 WiFi 访问/推送数据
 *  URL 列表：
 *    GET  /            -> 网页查看当前天气数据
 *    GET  /api/weather -> 返回 JSON 天气数据
 *    POST /api/weather  -> 电脑推送温湿度（JSON格式: {"temp":25.5,"hum":65}）
 *    GET  /api/stm32/force -> 强制立刻发一次数据给 STM32
 * ============================================================ */

/* GET / 根页面 */
static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char* html =
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<title>ESP32 天气数据</title>"
        "<meta http-equiv='refresh' content='5'>"
        "<style>body{font-family:Arial;max-width:600px;margin:50px auto;padding:20px}"
        ".box{background:#f0f8ff;padding:20px;border-radius:10px}"
        ".big{font-size:48px;color:#2c3e50}"
        ".ok{color:#27ae60}.err{color:#e74c3c}</style></head><body>"
        "<h1>🌤 ESP32 天气采集节点</h1>"
        "<div class='box'>"
        "<h3>当前数据（自动刷新5秒）</h3>"
        "<p><span class='big'>🌡 温度: %.2f °C</span></p>"
        "<p><span class='big'>💧 湿度: %.2f %%</span></p>"
        "<p>状态: <b class='%s'>%s</b></p>"
        "<p>更新时间戳: %lu</p>"
        "</div><hr>"
        "<h3>🛠 手动操作</h3>"
        "<p><a href='/api/stm32/force'>📤 立即发送一次数据到STM32</a></p>"
        "<p><a href='/api/weather'>🔍 查看JSON数据</a></p>"
        "<h3>📡 电脑推送数据 (POST /api/weather)</h3>"
        "<code>curl -X POST http://%s/api/weather -H \"Content-Type: application/json\" -d '{\"temp\":25.5,\"hum\":65.2}'</code>"
        "</body></html>";

    char buf[2048];
    const char* status_str;
    const char* cls_str;
    char ip_str[32];

    if (g_weather.valid) {
        status_str = "✅ 有效";
        cls_str = "ok";
    } else {
        status_str = "❌ 无效（请先通过API推送或等待天气拉取";
        cls_str = "err";
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));

    int len = snprintf(buf, sizeof(buf), html,
                       g_weather.temperature,
                       g_weather.humidity,
                       cls_str, status_str,
                       (unsigned long)g_weather.update_ts,
                       ip_str);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, buf, len);
    return ESP_OK;
}

/* GET /api/weather 返回JSON */
static esp_err_t api_weather_get_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "valid", g_weather.valid);
    cJSON_AddNumberToObject(root, "temp", g_weather.temperature);
    cJSON_AddNumberToObject(root, "hum", g_weather.humidity);
    cJSON_AddNumberToObject(root, "ts", g_weather.update_ts);
    char *json_str = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json_str, strlen(json_str));
    cJSON_free(json_str);
    cJSON_Delete(root);
    return ESP_OK;
}

/* POST /api/weather 电脑推送温湿度 */
static esp_err_t api_weather_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret, remaining = req->content_len;
    if (remaining >= sizeof(buf)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Content too long");
        return ESP_FAIL;
    }
    ret = httpd_req_recv(req, buf, remaining);
    if (ret <= 0) {
        if (ret == HTTPD_SOCK_ERR_TIMEOUT) {
            httpd_resp_send_408(req);
        }
        return ESP_FAIL;
    }
    buf[ret] = '\0';
    ESP_LOGI(TAG, "POST /api/weather body: %s", buf);

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid JSON");
        return ESP_FAIL;
    }
    cJSON *jtemp = cJSON_GetObjectItem(root, "temp");
    cJSON *jhum  = cJSON_GetObjectItem(root, "hum");
    if (!jtemp || !jhum || !cJSON_IsNumber(jtemp) || !cJSON_IsNumber(jhum)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Need temp and hum numbers");
        return ESP_FAIL;
    }

    float t = (float)cJSON_GetNumberValue(jtemp);
    float h = (float)cJSON_GetNumberValue(jhum);

    if (xSemaphoreTake(g_weather.lock, portMAX_DELAY) == pdTRUE) {
        g_weather.temperature = t;
        g_weather.humidity = h;
        g_weather.valid = true;
        g_weather.update_ts = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);
        xSemaphoreGive(g_weather.lock);
    }

    /* 收到新数据立刻转发 STM32 */
    uart_send_to_stm32(t, h);

    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    const char *resp = "{\"ok\":true,\"msg\":\"data received and sent to STM32\"}";
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

/* GET /api/stm32/force 强制发一次 */
static esp_err_t api_stm32_force_handler(httpd_req_t *req)
{
    if (!g_weather.valid) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No valid weather data yet");
        return ESP_FAIL;
    }
    uart_send_to_stm32(g_weather.temperature, g_weather.humidity);
    const char *resp = "{\"ok\":true,\"msg\":\"sent to STM32\"}";
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, resp, strlen(resp));
    return ESP_OK;
}

static const httpd_uri_t uri_root = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = root_get_handler,
    .user_ctx  = NULL
};
static const httpd_uri_t uri_api_get = {
    .uri       = "/api/weather",
    .method    = HTTP_GET,
    .handler   = api_weather_get_handler,
    .user_ctx  = NULL
};
static const httpd_uri_t uri_api_post = {
    .uri       = "/api/weather",
    .method    = HTTP_POST,
    .handler   = api_weather_post_handler,
    .user_ctx  = NULL
};
static const httpd_uri_t uri_stm32_force = {
    .uri       = "/api/stm32/force",
    .method    = HTTP_GET,
    .handler   = api_stm32_force_handler,
    .user_ctx  = NULL
};

static httpd_handle_t start_webserver(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = HTTP_SERVER_PORT;
    config.max_uri_handlers = 8;

    ESP_LOGI(TAG, "Starting HTTP Server on port: '%d'", config.server_port);
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_register_uri_handler(server, &uri_root);
        httpd_register_uri_handler(server, &uri_api_get);
        httpd_register_uri_handler(server, &uri_api_post);
        httpd_register_uri_handler(server, &uri_stm32_force);
        ESP_LOGI(TAG, "HTTP Server started OK");
        return server;
    }
    ESP_LOGI(TAG, "Error starting server!");
    return NULL;
}

/* ============================================================
 *  HTTP Client：从和风天气 API 拉取东莞天气数据
 *  每 5 分钟拉取一次
 * ============================================================ */
static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    return ESP_OK;
}

static bool fetch_weather_from_qweather(float *out_temp, float *out_hum)
{
    if (strcmp(QWEATHER_KEY, "YOUR_QWEATHER_API_KEY") == 0) {
        ESP_LOGW(TAG, "未配置和风天气API Key，跳过拉取天气模式");
        return false;
    }
    char url[256];
    snprintf(url, sizeof(url),
             "https://devapi.qweather.com/v7/weather/now?location=%s&key=%s",
             QWEATHER_LOCATION, QWEATHER_KEY);

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    bool ok = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int len = esp_http_client_get_content_length(client);
        if (status == 200 && len < 4096) {
            if (len <= 0) len = 4095;
            char *buf = malloc(len + 1);
            if (buf) {
                int r = esp_http_client_read(client, buf, len);
                if (r > 0) {
                    buf[r] = '\0';
                    ESP_LOGI(TAG, "QWeather resp: %s", buf);
                    cJSON *root = cJSON_Parse(buf);
                    if (root) {
                        cJSON *code = cJSON_GetObjectItem(root, "code");
                        if (code && strcmp(cJSON_GetStringValue(code), "200") == 0) {
                            cJSON *now = cJSON_GetObjectItem(root, "now");
                            if (now) {
                                cJSON *temp = cJSON_GetObjectItem(now, "temp");
                                cJSON *hum  = cJSON_GetObjectItem(now, "humidity");
                                if (temp && hum) {
                                    *out_temp = atof(cJSON_GetStringValue(temp));
                                    *out_hum  = atof(cJSON_GetStringValue(hum));
                                    ok = true;
                                    ESP_LOGI(TAG, "拉取天气成功: T=%.2f H=%.2f",
                                             *out_temp, *out_hum);
                                }
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                free(buf);
            }
        } else {
            ESP_LOGE(TAG, "HTTP GET failed, status=%d len=%d", status, len);
        }
    } else {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    return ok;
}

/* 简化版：wttr.in 不需要API Key，JSON格式简单，但国内可能慢 */
static bool fetch_weather_wttr(float *out_temp, float *out_hum)
{
    /* 东莞市经纬度：凤岗镇大约 22.78N 114.11E */
    const char *url = "https://wttr.in/Dongguan?format=j1";

    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 15000,
        .skip_cert_common_name_check = true,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);

    bool ok = false;
    if (err == ESP_OK) {
        int status = esp_http_client_get_status_code(client);
        int len = esp_http_client_get_content_length(client);
        if (status == 200 && len < 16384) {
            if (len <= 0) len = 16383;
            char *buf = malloc(len + 1);
            if (buf) {
                int r = esp_http_client_read(client, buf, len);
                if (r > 0) {
                    buf[r] = '\0';
                    cJSON *root = cJSON_Parse(buf);
                    if (root) {
                        cJSON *cur = cJSON_GetObjectItem(root, "current_condition");
                        if (cur && cJSON_IsArray(cur) && cJSON_GetArraySize(cur) > 0) {
                            cJSON *item = cJSON_GetArrayItem(cur, 0);
                            cJSON *temp = cJSON_GetObjectItem(item, "temp_C");
                            cJSON *hum  = cJSON_GetObjectItem(item, "humidity");
                            if (temp && hum) {
                                *out_temp = atof(cJSON_GetStringValue(temp));
                                *out_hum  = atof(cJSON_GetStringValue(hum));
                                ok = true;
                            }
                        }
                        cJSON_Delete(root);
                    }
                }
                free(buf);
            }
        }
    }
    esp_http_client_cleanup(client);
    return ok;
}

/* ============================================================
 *  FreeRTOS 任务
 * ============================================================ */

/* 任务1：LED 状态指示 + 周期天气拉取
 * 频率：每90秒一次 → 每天960次，和风天气免费1000次/天，剩40次冗余（失败重试用）
 * 额度计算：86400秒/天 ÷ 90秒/次 = 960次/天 < 1000次 ✅
 * 想要再稳一点改成120秒（2分钟）→ 720次/天，剩280次冗余 */
static void task_weather_pull(void *pvParam)
{
    /* 上次拉取天气的时间戳（秒） */
    uint32_t last_pull_sec = 0;
    const uint32_t PULL_INTERVAL_SEC = 90; /* 90秒一次，最快安全频率 */

    while (1) {
        uint32_t now_sec = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS / 1000);

        /* 到时间了（或第一次启动），拉取天气 */
        if (now_sec - last_pull_sec >= PULL_INTERVAL_SEC || last_pull_sec == 0) {
            ESP_LOGI(TAG, "⏰ 到点拉取天气（间隔%lu秒，每天约%lu次）...",
                     (unsigned long)PULL_INTERVAL_SEC,
                     (unsigned long)(86400 / PULL_INTERVAL_SEC));
            float t = 0, h = 0;
            bool got = false;
            /* 优先和风天气，失败就试 wttr.in */
            if (!got) got = fetch_weather_from_qweather(&t, &h);
            if (!got) got = fetch_weather_wttr(&t, &h);

            /* 无论成功失败，都更新时间戳，防止疯狂重试占带宽 */
            last_pull_sec = now_sec;

            if (got) {
                if (xSemaphoreTake(g_weather.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                    g_weather.temperature = t;
                    g_weather.humidity = h;
                    g_weather.valid = true;
                    g_weather.update_ts = now_sec;
                    xSemaphoreGive(g_weather.lock);
                }
                /* 拉到就立刻发STM32 */
                uart_send_to_stm32(t, h);
            } else {
                ESP_LOGW(TAG, "❌ 本次拉取天气失败，%lu秒后再试",
                         (unsigned long)PULL_INTERVAL_SEC);
            }
        }
        /* LED 快闪=无数据，慢闪=有数据 */
        if (g_weather.valid) {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(100));
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(2900));
        } else {
            gpio_set_level(BLINK_GPIO, 1);
            vTaskDelay(pdMS_TO_TICKS(200));
            gpio_set_level(BLINK_GPIO, 0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

/* 任务2：周期性发数据给 STM32（10秒一次，冗余确保校准有数据） */
static void task_uart_stm32(void *pvParam)
{
    while (1) {
        if (g_weather.valid) {
            float t, h;
            if (xSemaphoreTake(g_weather.lock, pdMS_TO_TICKS(100)) == pdTRUE) {
                t = g_weather.temperature;
                h = g_weather.humidity;
                xSemaphoreGive(g_weather.lock);
                uart_send_to_stm32(t, h);
            }
        } else {
            ESP_LOGW(TAG, "暂无有效天气数据，不发送STM32");
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

/* ============================================================
 *  app_main 入口
 * ============================================================ */
void app_main(void)
{
    /* LED 初始化 */
    gpio_reset_pin(BLINK_GPIO);
    gpio_set_direction(BLINK_GPIO, GPIO_MODE_OUTPUT);

    /* 天气数据锁 */
    g_weather.lock = xSemaphoreCreateMutex();

    /* UART2 初始化（与 STM32 通讯） */
    uart_config_t uart_config = {
        .baud_rate = UART_STM32_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_STM32_NUM, UART_BUF_SIZE * 2, UART_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_STM32_NUM, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(UART_STM32_NUM,
                                UART_STM32_TX_PIN,
                                UART_STM32_RX_PIN,
                                UART_PIN_NO_CHANGE,
                                UART_PIN_NO_CHANGE));
    ESP_LOGI(TAG, "UART2 init: TX=%d RX=%d baud=%d",
             UART_STM32_TX_PIN, UART_STM32_RX_PIN, UART_STM32_BAUD);

    /* NVS + WiFi */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    ESP_LOGI(TAG, "ESP_WIFI_MODE_STA");
    wifi_init_sta();

    /* 启动 HTTP Server */
    start_webserver();

    /* 打印 IP 提示 */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    esp_netif_get_ip_info(netif, &ip_info);
    ESP_LOGI(TAG, "================================================");
    ESP_LOGI(TAG, "✅ WiFi 连接成功！");
    ESP_LOGI(TAG, "🌐 ESP32 IP: " IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "📂 电脑浏览器打开: http://" IPSTR, IP2STR(&ip_info.ip));
    ESP_LOGI(TAG, "📤 POST 推送: curl -X POST http://" IPSTR "/api/weather ...");
    ESP_LOGI(TAG, "================================================");

    /* 启动任务 */
    xTaskCreate(task_weather_pull, "weather_pull", 8192, NULL, 5, NULL);
    xTaskCreate(task_uart_stm32, "uart_stm32", 4096, NULL, 4, NULL);
}