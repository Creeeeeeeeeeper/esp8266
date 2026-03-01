# ESP8266 Weather Clock

基于 ESP8266 + ST7735 1.44 寸 TFT 屏幕 (128x128) 的天气时钟。通过和风天气 API 获取实时天气数据，NTP 自动同步时间，WiFi 射频按需开关以降低功耗。

![7b5e02aed80f12e710029305b2baf659](./README/7b5e02aed80f12e710029305b2baf659.jpg)

Setup:

https://github.com/user-attachments/assets/f805d000-7f82-48c8-ac87-67825a61e22a

Update:

https://github.com/user-attachments/assets/19adfc11-88fd-4c7d-bcf6-531b0ce6d1d4

## 功能

- **实时时钟** - NTP 自动校时（每 1 小时），大字体显示时分，小字体显示秒，逐字符对比刷新无闪烁
- **日期显示** - 年/月/日 + 星期几，仅日期变化时重绘
- **实时天气** - 温度、湿度、风向、风力等级，每 10 分钟自动更新
- **18x18 像素天气图标** - 18 种手绘图标覆盖晴/云/阴/雨/雷暴/雪/雨夹雪/冻雨/雾/霾/沙尘/沙尘暴等天气
- **风向小旗** - 像素绘制的风向标，支持 8 方向
- **省电设计** - WiFi 射频仅在更新时开启，空闲时完全关闭；`WiFi.persistent(false)` 避免频繁写 flash导致寿命缩短
- **零堆碎片** - 天气数据使用固定大小 `char[]` 缓冲区，避免 Arduino `String` 动态分配导致的堆碎片化，提升长期运行稳定性
- **开机容错** - WiFi 连接失败自动重试 3 次，全部失败后显示错误提示并停机

## 硬件

| 组件 | 型号 |
|------|------|
| 主控 | ESP8266 (NodeMCU / D1 Mini) |
| 屏幕 | ST7735 1.44 寸 128x128 TFT |

### 接线

| TFT 引脚 | ESP8266 引脚 | 说明 |
|-----------|-------------|------|
| CS | D1 (GPIO5) | 片选 |
| DC | D3 (GPIO0) | 数据/命令 |
| RST | D2 (GPIO4) | 复位 |
| LED | D4 (GPIO2) | 背光 |
| SCK | D5 (GPIO14) | SPI 时钟 |
| SDA | D7 (GPIO13) | SPI 数据 |
| VCC | 3.3V | 电源 |
| GND | GND | 地线 |

![IMG_20260302_003932](./README/IMG_20260302_003932.jpg)

![IMG_20260302_004002](./README/IMG_20260302_004002.jpg)

## 依赖库

通过 Arduino IDE 库管理器安装：

- **Adafruit GFX Library**
- **Adafruit ST7735 and ST7789 Library**
- **ArduinoJson** (by Benoit Blanchon)

ESP8266 开发板自带（无需额外安装）：

- ESP8266WiFi
- ESP8266HTTPClient
- WiFiClientSecure

项目自带（无需安装）：

- **uzlib** - Gzip 解压（`uzlib.h` + `uzlib.c`，和风天气 API 返回 Gzip 压缩数据）

## 配置

编辑 `tft_weather_clock.ino` 中以下参数：

### WiFi

```cpp
const char* ssid     = "你的WiFi名称";
const char* password = "你的WiFi密码";
```

### 和风天气 API

需要在 [和风天气开发平台](https://dev.qweather.com/) 注册并创建凭据。

```cpp
const char* qweatherApiKey  = "你的API Key";
const char* qweatherHost    = "你的API Host";  // 在控制台-设置中查看
const char* locationID      = "101120601";      // 城市ID
const char* cityName        = "Weifang";        // 屏幕显示的城市名
```

API Host 在 [控制台-设置](https://console.qweather.com/setting) 中查看，格式类似 `xxxxxx.re.qweatherapi.com`。

### 刷新间隔

```cpp
const unsigned long WEATHER_INTERVAL = 600000;   // 天气刷新: 10 分钟
const unsigned long NTP_INTERVAL     = 3600000;  // NTP 校时: 1 小时
const unsigned long CLOCK_INTERVAL   = 1000;     // 时钟刷新: 1 秒
```

### 城市 ID

常用城市 ID：

| 城市 | ID |
|------|----|
| 北京 | 101010100 |
| 上海 | 101020100 |
| 广州 | 101280101 |
| 深圳 | 101280601 |
| 潍坊 | 101120601 |

其他城市可通过和风天气 [GeoAPI](https://dev.qweather.com/docs/api/geoapi/) 查询。

## 运行流程

### 开机流程

```
上电
 ├─ 初始化屏幕和背光
 ├─ WiFi.persistent(false) — 禁止写 flash
 ├─ 连接 WiFi（最多 3 次，每次 15 秒超时）
 │   ├─ 成功 → NTP 校时 → 获取天气 → 关闭 WiFi → 绘制界面 → 进入主循环
 │   └─ 3 次全失败 → 关闭 WiFi → 屏幕显示错误信息 → 停机（按 RST 重启）
 └─
```

### 主循环

```
loop() 每次循环
 ├─ 每 1 秒: 更新时钟显示（逐字符对比，无闪烁）
 ├─ 每 1 分钟: 更新 "Updated Xmin ago" 显示
 └─ 每 10 分钟 / 每 1 小时:
     ├─ 开启 WiFi 射频
     ├─ WiFi.begin() 连接（15 秒超时）
     ├─ 成功: 更新天气 / NTP 校时
     ├─ 失败: 标记失败，跳过本次
     └─ 关闭 WiFi 射频，回到省电模式
```

### WiFi 失败屏幕

开机 3 次连接全部失败时显示：

```
WiFi connect failed

Please check:
- SSID & password
- Router power
- Signal range

Restart to retry
```

## 天气图标

18x18 像素手绘图标，共 18 种样式，覆盖和风天气大部分图标代码：

| 图标 | 和风代码 | 说明 |
|------|---------|------|
| 晴 | 100, 150 | ![image-20260301225620529](./README/image-20260301225620529.png) |
| 白天多云 | 101-103 | ![image-20260301225710380](./README/image-20260301225710380.png) |
| 夜间多云 | 151-153 | ![image-20260301225737828](./README/image-20260301225737828.png) |
| 阴天 | 104 | ![image-20260301225756916](./README/image-20260301225756916.png) |
| 小雨/阵雨 | 300, 301, 305, 309, 350, 351 | ![image-20260301225819327](./README/image-20260301225819327.png) |
| 雷阵雨 | 302-304 | ![image-20260301225858623](./README/image-20260301225858623.png) |
| 中雨 | 306, 314, 315, 399 | ![image-20260301225918145](./README/image-20260301225918145.png) |
| 大雨/暴雨 | 307-312, 316-318 | ![image-20260301225937116](./README/image-20260301225937116.png) |
| 冻雨 | 313 | ![image-20260301230315816](./README/image-20260301230315816.png) |
| 小雪 | 400 | ![image-20260301230336539](./README/image-20260301230336539.png) |
| 中雪 | 401, 408 | ![image-20260301230358240](./README/image-20260301230358240.png) |
| 大雪/暴雪 | 402, 403, 409, 410, 499 | ![image-20260301230421297](./README/image-20260301230421297.png) |
| 雨夹雪 | 404-407, 456, 457 | ![image-20260301230440045](./README/image-20260301230440045.png) |
| 雾 | 500, 501, 509, 510, 514, 515 | ![image-20260301230504549](./README/image-20260301230504549.png) |
| 霾 | 502, 511-513 | ![image-20260301230523644](./README/image-20260301230523644.png) |
| 扬沙/浮尘 | 503, 504 | ![image-20260301230600048](./README/image-20260301230600048.png) |
| 沙尘暴 | 507, 508 | ![image-20260301230619533](./README/image-20260301230619533.png) |
| 未知 | 其他代码 | ![image-20260301230646258](./README/image-20260301230646258.png) |

全部为手工绘制，可能比较丑，有需要的可以自行重新绘制

## 文件结构

```
tft_weather_clock/
  tft_weather_clock.ino   # 主程序
  uzlib.h                 # Gzip 解压库头文件
  uzlib.c                 # Gzip 解压库源码
  README.md               # 本文件
```
