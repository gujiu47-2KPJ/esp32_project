#include "main.h"
#include "aqi_processor.h"
#include "aqi_client.h"
#include "uart_comm.h"
#include <math.h>

static const char *TAG = "AQI_PROCESSOR";

#define MQ135_R0        10.0
#define MQ135_A         116.6
#define MQ135_B         2.77
#define MQ135_ALPHA     0.002
#define MQ135_BETA      0.001

float calculate_indoor_aqi(float mq135_ppm, float temperature, float humidity)
{
    float temp_comp = 1.0 + MQ135_ALPHA * (temperature - 25.0);
    float humi_comp = 1.0 + MQ135_BETA * (humidity - 50.0);
    float compensated_ppm = mq135_ppm * temp_comp * humi_comp;
    
    ESP_LOGI(TAG, "MQ135 Raw: %.2f PPM, Compensated: %.2f PPM", 
             mq135_ppm, compensated_ppm);
    
    float iaqi = 0;
    
    if (compensated_ppm <= 0.1) {
        iaqi = compensated_ppm * 500;
    } else if (compensated_ppm <= 0.5) {
        iaqi = 50 + (compensated_ppm - 0.1) * 125;
    } else if (compensated_ppm <= 1.0) {
        iaqi = 100 + (compensated_ppm - 0.5) * 100;
    } else if (compensated_ppm <= 2.0) {
        iaqi = 150 + (compensated_ppm - 1.0) * 50;
    } else {
        iaqi = 200 + (compensated_ppm - 2.0) * 25;
    }
    
    if (iaqi > 500) iaqi = 500;
    if (iaqi < 0) iaqi = 0;
    
    return iaqi;
}

static void get_aqi_level(float aqi, char *level, size_t level_size)
{
    if (aqi <= 50) {
        strncpy(level, "优", level_size);
    } else if (aqi <= 100) {
        strncpy(level, "良", level_size);
    } else if (aqi <= 150) {
        strncpy(level, "轻度污染", level_size);
    } else if (aqi <= 200) {
        strncpy(level, "中度污染", level_size);
    } else if (aqi <= 300) {
        strncpy(level, "重度污染", level_size);
    } else {
        strncpy(level, "严重污染", level_size);
    }
    level[level_size - 1] = '\0';
}

void generate_suggestion(comprehensive_aqi_data_t *data)
{
    char suggestion[256] = {0};
    
    float indoor_aqi = data->indoor.calculated_aqi;
    float outdoor_aqi = data->outdoor.aqi;
    
    if (indoor_aqi < outdoor_aqi) {
        snprintf(suggestion, sizeof(suggestion),
                 "室内空气质量(%s)优于室外(%s)。\n"
                 "建议：关闭门窗，使用空气净化器保持室内空气质量。",
                 data->indoor.level, data->outdoor.level);
    } else if (indoor_aqi > outdoor_aqi * 1.5) {
        snprintf(suggestion, sizeof(suggestion),
                 "室内空气质量(%s)差于室外(%s)。\n"
                 "建议：开窗通风，检查室内污染源（烹饪、吸烟等）。",
                 data->indoor.level, data->outdoor.level);
    } else {
        snprintf(suggestion, sizeof(suggestion),
                 "室内外空气质量相近（室内%s，室外%s）。\n"
                 "建议：可适当开窗通风，保持空气流通。",
                 data->indoor.level, data->outdoor.level);
    }
    
    if (indoor_aqi > 150) {
        strncat(suggestion, "\n警告：室内污染严重，请立即采取措施！", 
                sizeof(suggestion) - strlen(suggestion) - 1);
    } else if (indoor_aqi > 100) {
        strncat(suggestion, "\n注意：室内空气质量不佳，敏感人群应减少活动。",
                sizeof(suggestion) - strlen(suggestion) - 1);
    }
    
    strncpy(data->final_suggestion, suggestion, sizeof(data->final_suggestion) - 1);
    
    snprintf(data->comparison_result, sizeof(data->comparison_result),
             "室内AQI: %.0f (%s)\n"
             "室外AQI: %.0f (%s)\n"
             "差值: %.0f",
             indoor_aqi, data->indoor.level,
             outdoor_aqi, data->outdoor.level,
             indoor_aqi - outdoor_aqi);
    
    ESP_LOGI(TAG, "Suggestion generated");
}

void aqi_processor_task(void *pvParameters)
{
    comprehensive_aqi_data_t comprehensive_data;
    indoor_aqi_data_t indoor_data;
    outdoor_aqi_data_t outdoor_data;
    
    ESP_LOGI(TAG, "AQI processor task started");
    
    while(1) {
        EventBits_t bits = xEventGroupWaitBits(
            aqi_event_group,
            AQI_FETCHED_BIT,
            pdTRUE,
            pdTRUE,
            pdMS_TO_TICKS(5000)
        );
        
        if (bits & AQI_FETCHED_BIT) {
            ESP_LOGI(TAG, "Processing air quality data...");
            
            if (receive_from_stm32(&indoor_data) == ESP_OK) {
                indoor_data.calculated_aqi = calculate_indoor_aqi(
                    indoor_data.mq135_ppm,
                    indoor_data.temperature,
                    indoor_data.humidity
                );
                
                get_aqi_level(indoor_data.calculated_aqi, 
                              indoor_data.level, sizeof(indoor_data.level));
                
                comprehensive_data.indoor = indoor_data;
                
                generate_suggestion(&comprehensive_data);
                
                send_to_stm32(comprehensive_data.final_suggestion, 
                              strlen(comprehensive_data.final_suggestion));
                
                xEventGroupSetBits(aqi_event_group, DATA_PROCESSED_BIT);
                
                ESP_LOGI(TAG, "Data processing completed");
            }
        }
        
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}