# ESP32-S3 + STM32 空气质量监测系统

## 系统架构

```
┌─────────────────────────────────────────────────────────────┐
│                      系统数据流                              │
└─────────────────────────────────────────────────────────────┘

MQ-135传感器 ──→ STM32(采集) ──[UART 115200]──→ ESP32-S3
                                                      │
                                                      ↓
                                              WiFi获取室外数据
                                              (OpenWeatherMap API)
                                                      │
                                                      ↓
                                              分析室内外差异
                                              生成健康评估
                                                      │
                                                      ↓
ESP32-S3 ──[UART 115200]──→ STM32 ──→ OLED显示建议信息
```

## 硬件连接

### ESP32-S3 ↔ STM32 UART连接

| ESP32-S3 | 连接 | STM32 |
|----------|------|-------|
| GPIO4 (TX) | ──→ | USART1_RX (PA10) |
| GPIO5 (RX) | ←── | USART1_TX (PA9) |
| GND | ──→ | GND |

**注意**：确保共地连接！

### STM32端已配置
- USART1: 115200波特率, 8N1
- I2C1: OLED显示
- ADC1: MQ-135传感器

## 软件配置

### 1. 修改WiFi配置

打开 `hello_world_main.c`，修改以下宏定义：

```c
#define WIFI_SSID      "你的WiFi名称"
#define WIFI_PASSWORD  "你的WiFi密码"
```

### 2. 获取OpenWeatherMap API Key

1. 访问 https://openweathermap.org/api/air-pollution
2. 注册免费账号
3. 获取API Key
4. 修改代码中的 `WEATHER_API_KEY`

### 3. 修改位置坐标

```c
/* 查询你所在城市的经纬度：https://www.latlong.net/ */
#define WEATHER_API_URL "http://api.openweathermap.org/data/2.5/air_pollution?lat=你的纬度&lon=你的经度&appid=" WEATHER_API_KEY
```

### 4. 编译和烧录

```bash
# 进入项目目录
cd g:\esp32project\hello_world

# 配置项目（选择ESP32-S3目标）
idf.py set-target esp32s3

# 编译
idf.py build

# 烧录（替换为你的串口端口）
idf.py -p COM3 flash

# 监视串口输出
idf.py -p COM3 monitor
```

## 通信协议

### STM32 → ESP32 数据格式

```
MQ135:PPM=450.00,TEMP=25.00,HUMI=50.00,AQI=450.00,LEVEL=Good\r\n
```

**字段说明**：
- `PPM`: CO2浓度 (ppm)
- `TEMP`: 温度 (预留，当前固定25.00)
- `HUMI`: 湿度 (预留，当前固定50.00)
- `AQI`: 空气质量指数
- `LEVEL`: 空气质量等级 (Excellent/Good/Moderate/Poor/Bad/Hazardous)

### ESP32 → STM32 数据格式

```
建议文本字符串\n
```

**示例**：
```
Good time to ventilate room\n
Open windows! Use air purifier!\n
Excellent air quality!\n
```

**注意**：以换行符 `\n` 结尾，STM32端通过检测 `\n` 判断消息结束。

## 空气质量分析算法

### 室内AQI计算模型

基于CO2浓度的简化AQI计算：

| CO2浓度 (PPM) | AQI范围 | 空气质量等级 |
|---------------|---------|-------------|
| < 400 | 0-50 | Excellent (优秀) |
| 400-1000 | 50-100 | Good (良好) |
| 1000-2000 | 100-200 | Moderate (一般) |
| > 2000 | 200+ | Poor/Bad (差) |

### 通风建议算法

根据室内外AQI差异提供建议：

| AQI差异 (室内-室外) | 通风评分 | 建议 |
|---------------------|---------|------|
| > 50 | 90% | 强烈建议通风 |
| 20-50 | 70% | 建议通风 |
| -20 ~ 20 | 50% | 可适度通风 |
| -50 ~ -20 | 30% | 谨慎通风 |
| < -50 | 10% | 不建议通风 |

### 健康评估等级

| 室内AQI | 健康等级 | 说明 |
|---------|---------|------|
| 0-50 | 1 - Excellent | 空气质量优秀，适合所有人群 |
| 51-100 | 2 - Good | 空气质量良好，敏感人群需注意 |
| 101-150 | 3 - Moderate | 敏感人群应减少户外活动 |
| 151-200 | 4 - Poor | 所有人应减少户外活动 |
| 201-300 | 5 - Bad | 避免户外活动，使用空气净化器 |
| > 300 | 6 - Hazardous | 危险！立即采取防护措施 |

## 系统任务

ESP32-S3运行两个FreeRTOS任务：

### 1. STM32通信任务 (优先级5)
- 持续监听UART接收STM32数据
- 解析传感器数据
- 执行空气质量分析
- 发送建议到STM32

### 2. 室外数据获取任务 (优先级4)
- 连接WiFi
- 每10分钟调用OpenWeatherMap API
- 解析室外空气质量数据
- 更新全局数据供分析任务使用

## 日志输出示例

```
I (0) AIR_QUALITY: ESP32-S3 Air Quality Analysis System Starting...
I (100) AIR_QUALITY: UART initialized on GPIO4(TX), GPIO5(RX)
I (200) AIR_QUALITY: Connecting to WiFi...
I (1500) AIR_QUALITY: got ip:192.168.1.100
I (1500) AIR_QUALITY: connected to ap SSID:MyWiFi
I (2000) AIR_QUALITY: System ready. Waiting for STM32 data...
I (2100) AIR_QUALITY: Sent to STM32: ESP32 Ready
I (3000) AIR_QUALITY: Outdoor data fetch task started
I (3000) AIR_QUALITY: Fetching outdoor air quality data...
I (4000) AIR_QUALITY: HTTP GET request completed successfully
I (4000) AIR_QUALITY: Outdoor AQI: 3 (PM2.5: 12.5, PM10: 25.3)
I (5000) AIR_QUALITY: STM32 communication task started
I (5500) AIR_QUALITY: Received from STM32: MQ135:PPM=650.00,TEMP=25.00,HUMI=50.00,AQI=650.00,LEVEL=Good
I (5500) AIR_QUALITY: Parsed - CO2: 650.00 PPM, AQI: 650.00, Level: Good
I (5500) AIR_QUALITY: === Air Quality Analysis ===
I (5500) AIR_QUALITY: Indoor AQI: 70
I (5500) AIR_QUALITY: Outdoor AQI: 3
I (5500) AIR_QUALITY: Difference: 67
I (5500) AIR_QUALITY: Ventilation Score: 90%
I (5500) AIR_QUALITY: Health Level: 2 - Good
I (5500) AIR_QUALITY: Suggestion: Ventilate room now! Open windows!
I (5500) AIR_QUALITY: ===========================
I (5500) AIR_QUALITY: Sent to STM32: Ventilate room now! Open windows!
```

## 故障排查

### WiFi连接失败
- 检查SSID和密码是否正确
- 确认WiFi信号强度
- 系统会在离线模式下继续工作（使用默认室外AQI=100）

### UART通信失败
- 检查TX/RX是否正确交叉连接
- 确认波特率配置一致（115200）
- 检查GND是否共地
- 使用逻辑分析仪或示波器检查信号

### API调用失败
- 检查API Key是否有效
- 确认网络连接正常
- 验证坐标格式正确
- 查看OpenWeatherMap账户配额

### OLED无显示
- 检查STM32端I2C连接
- 确认OLED地址正确（通常0x3C或0x3D）
- 检查STM32接收缓冲区是否正常

## 后续扩展建议

1. **添加DHT11/DHT22传感器**（STM32端）
   - 提供真实的温湿度数据
   - 改进空气质量分析精度

2. **增加更多气体检测**
   - MQ-2（烟雾/液化气）
   - MQ-7（CO）
   - MQ-135已支持多种气体

3. **数据持久化**
   - 使用ESP32的SPIFFS/LittleFS存储历史数据
   - 利用STM32的W25QXX Flash记录日志

4. **Web界面**
   - ESP32-S3搭建HTTP服务器
   - 实时显示空气质量图表
   - 历史数据查询

5. **智能控制**
   - 连接继电器控制排风扇
   - 自动控制空气净化器
   - MQTT接入智能家居系统

## 技术支持

- ESP-IDF文档：https://docs.espressif.com/projects/esp-idf/
- OpenWeatherMap API：https://openweathermap.org/api
- STM32 HAL库文档：https://www.st.com/en/microcontrollers-microprocessors/stm32-32-bit-arm-cortex-mcus.html