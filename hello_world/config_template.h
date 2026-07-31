/*
 * 配置文件模板
 * 
 * 使用说明：
 * 1. 将此文件复制为 config.h
 * 2. 修改下面的配置参数
 * 3. 在 hello_world_main.c 中包含此文件
 */

#ifndef CONFIG_H
#define CONFIG_H

/* ==================== WiFi配置 ==================== */
/* 请修改为你的WiFi名称和密码 */
#define WIFI_SSID      "Your_WiFi_SSID"
#define WIFI_PASSWORD  "Your_WiFi_Password"

/* ==================== 室外空气质量API配置 ==================== */
/* 
 * OpenWeatherMap API Key
 * 申请地址：https://openweathermap.org/api/air-pollution
 * 免费版支持：1000次/天调用
 */
#define WEATHER_API_KEY    "Your_OpenWeatherMap_API_Key_Here"

/* 
 * 位置坐标 (经纬度)
 * 请使用你所在城市的坐标
 * 查询地址：https://www.latlong.net/
 * 
 * 常见城市坐标：
 * 北京：39.9042, 116.4074
 * 上海：31.2304, 121.4737
 * 广州：23.1291, 113.2644
 * 深圳：22.5431, 114.0579
 */
#define LOCATION_LAT  "39.9042"  /* 纬度 */
#define LOCATION_LON  "116.4074" /* 经度 */

/* ==================== UART引脚配置 ==================== */
/* 
 * ESP32-S3与STM32通信的UART引脚
 * 请根据实际硬件连接修改
 * 
 * 推荐引脚（避免与Flash/PSRAM冲突）：
 * UART1: TX=GPIO4, RX=GPIO5
 * UART2: TX=GPIO15, RX=GPIO16
 */
#define STM_UART_TX_PIN  4  /* TX引脚 */
#define STM_UART_RX_PIN  5  /* RX引脚 */

/* ==================== 其他配置 ==================== */
/* 数据采集间隔（秒） */
#define SAMPLE_INTERVAL_SEC  10

/* WiFi最大重试次数 */
#define WIFI_MAX_RETRY  5

#endif /* CONFIG_H */