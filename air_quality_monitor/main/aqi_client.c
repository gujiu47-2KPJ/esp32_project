#include "main.h"
#include "aqi_client.h"
#include "esp_http_client.h"
#include "cJSON.h"

static const char *TAG = "AQI_CLIENT";

static char http_response_buffer[2048];
static int http_response_index = 0;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    switch(evt->event_id) {
        case HTTP_EVENT_ERROR:
            ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
            break;
        case HTTP_EVENT_ON_CONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
            break;
        case HTTP_EVENT_HEADER_SENT:
            ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
            break;
        case HTTP_EVENT_ON_HEADER:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", 
                     evt->header_key, evt->header_value);
            break;
        case HTTP_EVENT_ON_DATA:
            if (http_response_index + evt->data_len < sizeof(http_response_buffer)) {
                memcpy(http_response_buffer + http_response_index, 
                       evt->data, evt->data_len);
                http_response_index += evt->data_len;
            }
            break;
        case HTTP_EVENT_ON_FINISH:
            ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
            http_response_buffer[http_response_index] = '\0';
            break;
        case HTTP_EVENT_DISCONNECTED:
            ESP_LOGD(TAG, "HTTP_EVENT_DISCONNECTED");
            http_response_index = 0;
            break;
        default:
            break;
    }
    return ESP_OK;
}

void aqi_client_task(void *pvParameters)
{
    outdoor_aqi_data_t outdoor_data;
    
    xEventGroupWaitBits(aqi_event_group, WIFI_CONNECTED_BIT, 
                        pdFALSE, pdTRUE, portMAX_DELAY);
    
    ESP_LOGI(TAG, "Starting AQI client task...");
    
    while(1) {
        if (fetch_outdoor_aqi(&outdoor_data) == ESP_OK) {
            ESP_LOGI(TAG, "Outdoor AQI: %.0f, Level: %s", 
                     outdoor_data.aqi, outdoor_data.level);
            xEventGroupSetBits(aqi_event_group, AQI_FETCHED_BIT);
        } else {
            ESP_LOGE(TAG, "Failed to fetch outdoor AQI");
        }
        
        vTaskDelay(pdMS_TO_TICKS(600000));
    }
}

esp_err_t fetch_outdoor_aqi(outdoor_aqi_data_t *data)
{
    memset(http_response_buffer, 0, sizeof(http_response_buffer));
    http_response_index = 0;
    
    char url[256];
    snprintf(url, sizeof(url), 
             "http://api.waqi.info/feed/%s/?token=%s", 
             CITY_NAME, AQI_API_KEY);
    
    esp_http_client_config_t config = {
        .url = url,
        .event_handler = http_event_handler,
        .timeout_ms = 10000,
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "Failed to initialize HTTP client");
        return ESP_FAIL;
    }
    
    esp_err_t err = esp_http_client_perform(client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "HTTP GET request failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(client);
        return err;
    }
    
    int status_code = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "HTTP Status Code: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Invalid HTTP response: %d", status_code);
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    cJSON *root = cJSON_Parse(http_response_buffer);
    if (root == NULL) {
        ESP_LOGE(TAG, "Failed to parse JSON");
        esp_http_client_cleanup(client);
        return ESP_FAIL;
    }
    
    cJSON *status = cJSON_GetObjectItem(root, "status");
    if (status && cJSON_GetNumberValue(status) == 1) {
        cJSON *data_obj = cJSON_GetObjectItem(root, "data");
        if (data_obj) {
            cJSON *aqi = cJSON_GetObjectItem(data_obj, "aqi");
            if (aqi) {
                data->aqi = (float)cJSON_GetNumberValue(aqi);
            }
            
            cJSON *iaqi = cJSON_GetObjectItem(data_obj, "iaqi");
            if (iaqi) {
                cJSON *pm25 = cJSON_GetObjectItem(iaqi, "pm25");
                if (pm25) {
                    cJSON *val = cJSON_GetObjectItem(pm25, "v");
                    if (val) data->pm25 = (float)cJSON_GetNumberValue(val);
                }
                
                cJSON *pm10 = cJSON_GetObjectItem(iaqi, "pm10");
                if (pm10) {
                    cJSON *val = cJSON_GetObjectItem(pm10, "v");
                    if (val) data->pm10 = (float)cJSON_GetNumberValue(val);
                }
            }
            
            if (data->aqi <= 50) {
                strcpy(data->level, "优");
            } else if (data->aqi <= 100) {
                strcpy(data->level, "良");
            } else if (data->aqi <= 150) {
                strcpy(data->level, "轻度污染");
            } else if (data->aqi <= 200) {
                strcpy(data->level, "中度污染");
            } else if (data->aqi <= 300) {
                strcpy(data->level, "重度污染");
            } else {
                strcpy(data->level, "严重污染");
            }
        }
    }
    
    cJSON_Delete(root);
    esp_http_client_cleanup(client);
    
    return ESP_OK;
}