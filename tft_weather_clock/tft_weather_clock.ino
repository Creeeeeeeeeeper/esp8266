// ============ 天气时钟 - Weather Clock ============
// 基于 ESP8266 + ST7735 1.44寸TFT屏幕
// 功能: NTP时间同步 + 实时天气显示
// ================================================

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

extern "C" {
  #include "uzlib.h"
}

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>
#include <WiFiUdp.h>
#include <time.h>

// ============ 引脚配置 ============
#define TFT_CS   D1  // GPIO5
#define TFT_DC   D3  // GPIO0
#define TFT_RST  D2  // GPIO4
#define TFT_LED  D4  // GPIO2

// ============ WiFi 配置 ============
const char* ssid     = "";
const char* password = "";

// ============ 和风天气 API 配置 ============
// https://dev.qweather.com/
const char* qweatherApiKey  = "";
const char* qweatherHost    = "";
const char* locationID      = "101120601";   // 潍坊 (可在和风天气城市搜索API查询)
const char* cityName        = "Weifang";

// ============ NTP 配置 ============
const char* ntpServer = "ntp.aliyun.com";
const long  gmtOffset = 8 * 3600;  // UTC+8 北京时间
const int   dstOffset = 0;

// ============ 刷新间隔 ============
const unsigned long WEATHER_INTERVAL = 600000;   // 天气刷新: 10分钟
const unsigned long NTP_INTERVAL     = 3600000;  // NTP校时: 1小时
const unsigned long CLOCK_INTERVAL   = 1000;     // 时钟刷新: 1秒

// ============ 颜色定义 ============
#define COLOR_BG        0x0000  // 黑色背景
#define COLOR_TIME      0x07FF  // 青色 - 时间
#define COLOR_DATE      0xFFFF  // 白色 - 日期
#define COLOR_TEMP      0xFD20  // 橙色 - 温度
#define COLOR_WEATHER   0x07E0  // 绿色 - 天气描述
#define COLOR_HUMIDITY  0x5D1F  // 紫色 - 湿度
#define COLOR_WIND      0xB5F6  // 浅灰蓝 - 风速
#define COLOR_CITY      0xFFE0  // 黄色 - 城市名
#define COLOR_SECONDS   0x6B4D  // 中灰 - 秒数 (比分割线亮)
#define COLOR_DIVIDER   0x4208  // 深灰 - 分割线
#define COLOR_UPDATE    0x8C71  // 浅灰 - last update文字
#define COLOR_CONN      0x07E0  // 绿色 - 连接提示

// ============ 全局变量 ============
Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);

unsigned long lastWeatherUpdate = 0;
unsigned long lastNTPUpdate     = 0;
unsigned long lastClockUpdate   = 0;
int lastSecond = -1;
int lastMinute = -1;

// 上次时间字符串缓存 (用于无闪烁更新)
char lastTimeBuf[6] = "     ";  // "HH:MM"
char lastSecBuf[4]  = "   ";   // ":SS"

// 天气数据缓存
char   weatherDesc[16] = "--";
float  temperature   = 0;
float  humidity      = 0;
float  windSpeed     = 0;
char   windDir[12]     = "";
char   windScale[4]    = "";
int    weatherIconCode = 999;
bool   weatherValid  = false;
bool   weatherFailed = false;   // 上次更新是否失败

// last update 绘制状态缓存
bool lastUpdateDrawn = false;
bool lastUpdateFailed = false;
int  lastUpdateMinVal = -1;

// 星期
const char* weekDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

// ============ 初始化 ============
void setup() {
  Serial.begin(115200);
  Serial.println("\n=== 天气时钟启动 ===");

  // 初始化背光
  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  // 初始化屏幕
  tft.initR(INITR_144GREENTAB);
  tft.setRotation(0);
  tft.fillScreen(COLOR_BG);

  // 避免每次WiFi.begin都写flash, 保护flash寿命
  WiFi.persistent(false);

  // 连接WiFi (最多3次)
  if (!connectWiFi()) {
    // 3次全部失败, 显示错误信息并停机
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    tft.fillScreen(COLOR_BG);
    tft.setTextSize(1);
    tft.setTextColor(0xF800);
    tft.setCursor(4, 20);
    tft.print("WiFi connect failed");
    tft.setCursor(4, 40);
    tft.setTextColor(COLOR_DATE);
    tft.print("Please check:");
    tft.setCursor(4, 55);
    tft.setTextColor(COLOR_UPDATE);
    tft.print("- SSID & password");
    tft.setCursor(4, 68);
    tft.print("- Router power");
    tft.setCursor(4, 81);
    tft.print("- Signal range");
    tft.setCursor(4, 100);
    tft.setTextColor(COLOR_DIVIDER);
    tft.print("Restart to retry");
    Serial.println("WiFi连接彻底失败, 停机");
    while (true) { delay(1000); yield(); }
  }

  // 配置NTP时间
  configTime(gmtOffset, dstOffset, ntpServer, "pool.ntp.org", "time.nist.gov");
  Serial.println("等待NTP同步...");
  waitForNTP();

  // 首次获取天气
  updateWeather();

  // 初始设置完成, 断开WiFi省电
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.println("WiFi已断开, 省电模式");

  // 绘制完整界面
  drawFullUI();
}

// ============ 主循环 ============
void loop() {
  unsigned long now = millis();

  // 每秒更新时钟
  if (now - lastClockUpdate >= CLOCK_INTERVAL) {
    lastClockUpdate = now;
    updateClock();
  }

  // 每分钟更新 "last update" 显示
  static unsigned long lastUpdateDraw = 0;
  if (now - lastUpdateDraw >= 60000) {
    lastUpdateDraw = now;
    drawLastUpdate();
  }

  // 判断是否需要连WiFi (天气或NTP)
  bool needWeather = (now - lastWeatherUpdate >= WEATHER_INTERVAL);
  bool needNTP     = (now - lastNTPUpdate >= NTP_INTERVAL);

  // NTP必须搭天气的WiFi一起做, 避免单独为NTP开一次WiFi
  // 如果NTP到期但天气没到期, 等到下次天气更新时一起做
  if (needWeather) {
    // 天气到期, 顺便检查NTP是否也该做了
    // 显示更新中状态
    drawUpdating();

    // 连接WiFi
    Serial.println("连接WiFi进行更新...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
      delay(100);
      // 用 millis 判断是否该刷新时钟, 避免计时漂移
      unsigned long t = millis();
      if (t - lastClockUpdate >= CLOCK_INTERVAL) {
        lastClockUpdate = t;
        updateClock();
      }
      yield();
    }

    if (WiFi.status() == WL_CONNECTED) {
      // NTP校时 (搭便车, 如果到期了就一起做)
      if (needNTP) {
        configTime(gmtOffset, dstOffset, ntpServer, "pool.ntp.org", "time.nist.gov");
        delay(1000);
        lastNTPUpdate = millis();
        Serial.println("NTP校时完成");
      }

      // 天气更新
      updateWeather();
    } else {
      Serial.println("WiFi连接失败, 跳过本次更新");
      weatherFailed = true;
      lastWeatherUpdate = millis();
      if (needNTP) lastNTPUpdate = millis();
      drawLastUpdate();
    }

    // 断开WiFi省电
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    Serial.println("WiFi已断开, 省电模式");
  }
}

// ============ WiFi 连接 (最多尝试3次) ============
bool connectWiFi() {
  WiFi.mode(WIFI_STA);

  for (int attempt = 1; attempt <= 3; attempt++) {
    tft.fillScreen(COLOR_BG);
    tft.setTextSize(1);
    tft.setTextColor(COLOR_CONN);
    tft.setCursor(10, 30);
    tft.printf("WiFi Attempt %d/3", attempt);
    tft.setCursor(10, 45);
    tft.setTextColor(COLOR_DATE);
    tft.print(ssid);

    WiFi.begin(ssid, password);

    int dots = 0;
    unsigned long wifiStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 15000) {
      delay(500);
      Serial.print(".");
      tft.setCursor(10 + dots * 6, 60);
      tft.setTextColor(COLOR_CONN);
      tft.print(".");
      dots++;
      if (dots > 18) {
        dots = 0;
        tft.fillRect(10, 60, 118, 10, COLOR_BG);
      }
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("\nWiFi已连接! (第%d次尝试)\n", attempt);
      Serial.print("IP: ");
      Serial.println(WiFi.localIP());

      tft.fillRect(0, 55, 128, 20, COLOR_BG);
      tft.setCursor(10, 60);
      tft.setTextColor(COLOR_CONN);
      tft.print("WiFi Connected!");
      delay(1000);
      return true;
    }

    Serial.printf("\n第%d次连接失败\n", attempt);
    WiFi.disconnect(true);

    if (attempt < 3) {
      tft.fillRect(0, 55, 128, 20, COLOR_BG);
      tft.setCursor(10, 60);
      tft.setTextColor(0xF800);
      tft.print("Failed, retrying...");
      delay(2000);
    }
  }

  return false;
}

// ============ 等待NTP同步 ============
void waitForNTP() {
  tft.fillRect(0, 75, 128, 15, COLOR_BG);
  tft.setCursor(10, 78);
  tft.setTextColor(COLOR_TIME);
  tft.print("Syncing time...");

  struct tm timeinfo;
  int retry = 0;
  while (!getLocalTime(&timeinfo) && retry < 20) {
    delay(500);
    retry++;
    Serial.print(".");
  }

  if (retry < 20) {
    Serial.println("\nNTP同步成功!");
  } else {
    Serial.println("\nNTP同步超时, 将继续尝试");
  }
  delay(500);
}

// ============ 绘制完整界面 ============
void drawFullUI() {
  tft.fillScreen(COLOR_BG);

  // 绘制分割线
  tft.drawFastHLine(4, 52, 120, COLOR_DIVIDER);
  tft.drawFastHLine(4, 78, 120, COLOR_DIVIDER);

  // 绘制天气区域标签
  drawWeatherInfo();

  // 重置时间缓存, 强制全部重绘
  memset(lastTimeBuf, ' ', 5);
  lastTimeBuf[5] = '\0';
  memset(lastSecBuf, ' ', 3);
  lastSecBuf[3] = '\0';
  lastMinute = -1;
  lastSecond = -1;
  updateClock();
}

// ============ 更新时钟显示 (无闪烁, 逐字符对比) ============
void updateClock() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  int sec = timeinfo.tm_sec;
  int min = timeinfo.tm_min;
  int hour = timeinfo.tm_hour;

  // === HH:MM 大字体, 逐字符对比更新 ===
  if (min != lastMinute) {
    char timeBuf[6];
    sprintf(timeBuf, "%02d:%02d", hour, min);
    tft.setTextSize(3);
    // size=3: 每字符宽 6*3=18px, 高 8*3=24px
    int startX = 19;
    int startY = 6;
    for (int i = 0; i < 5; i++) {
      if (timeBuf[i] != lastTimeBuf[i]) {
        int cx = startX + i * 18;
        // 用背景色擦除旧字符
        tft.setTextColor(COLOR_BG, COLOR_BG);
        tft.setCursor(cx, startY);
        tft.print(lastTimeBuf[i]);
        // 用前景色绘制新字符
        tft.setTextColor(COLOR_TIME, COLOR_BG);
        tft.setCursor(cx, startY);
        tft.print(timeBuf[i]);
      }
    }
    memcpy(lastTimeBuf, timeBuf, 6);
    lastMinute = min;
  }

  // === 秒数 (小字体, 逐字符对比) ===
  if (sec != lastSecond) {
    char secBuf[4];
    sprintf(secBuf, ":%02d", sec);
    tft.setTextSize(1);
    // size=1: 每字符宽 6px, 高 8px
    int startX = 110;
    int startY = 22;
    for (int i = 0; i < 3; i++) {
      if (secBuf[i] != lastSecBuf[i]) {
        int cx = startX + i * 6;
        tft.setTextColor(COLOR_BG, COLOR_BG);
        tft.setCursor(cx, startY);
        tft.print(lastSecBuf[i]);
        tft.setTextColor(COLOR_SECONDS, COLOR_BG);
        tft.setCursor(cx, startY);
        tft.print(secBuf[i]);
      }
    }
    memcpy(lastSecBuf, secBuf, 4);
    lastSecond = sec;
  }

  // 日期行: 只在日期变化时更新
  static int lastDrawnDay = -1;
  if (timeinfo.tm_mday != lastDrawnDay) {
    lastDrawnDay = timeinfo.tm_mday;
    tft.fillRect(0, 38, 128, 14, COLOR_BG);

    char dateBuf[20];
    sprintf(dateBuf, "%d/%02d/%02d", timeinfo.tm_year + 1900,
            timeinfo.tm_mon + 1, timeinfo.tm_mday);

    tft.setTextSize(1);
    tft.setTextColor(COLOR_DATE, COLOR_BG);
    tft.setCursor(10, 40);
    tft.print(dateBuf);

    // 星期几
    tft.setCursor(94, 40);
    tft.print(weekDays[timeinfo.tm_wday]);
  }
}

// ============ 更新天气数据 ============
void updateWeather() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi未连接, 跳过天气更新");
    weatherFailed = true;
    lastWeatherUpdate = millis();
    drawLastUpdate();
    return;
  }

  Serial.println("\n====== 开始获取天气 ======");

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;

  String url = "https://";
  url += qweatherHost;
  url += "/v7/weather/now?location=";
  url += locationID;

  Serial.print("请求URL: ");
  Serial.println(url);

  Serial.println("正在连接服务器...");
  http.begin(client, url);
  http.addHeader("X-QW-Api-Key", qweatherApiKey);
  http.addHeader("Accept-Encoding", "gzip");

  Serial.println("发送GET请求...");
  int httpCode = http.GET();
  Serial.printf("HTTP状态码: %d\n", httpCode);

  if (httpCode == 200) {
    int len = http.getSize();
    Serial.printf("响应长度: %d\n", len);

    // 读取原始gzip数据
    WiFiClient* stream = http.getStreamPtr();
    static uint8_t gzBuf[512];
    int gzLen = 0;
    unsigned long timeout = millis();
    while (gzLen < (int)sizeof(gzBuf) && (millis() - timeout < 5000)) {
      if (stream->available()) {
        gzBuf[gzLen++] = stream->read();
        timeout = millis();
      } else if (gzLen > 0) {
        delay(10);
        if (!stream->available()) break;
      } else {
        delay(10);
      }
      yield();
    }
    Serial.printf("读取gzip数据: %d 字节\n", gzLen);

    // 使用 uzlib 解压
    static char jsonBuf[1024];
    memset(jsonBuf, 0, sizeof(jsonBuf));

    uzlib_init();

    static struct uzlib_uncomp d;
    uzlib_uncompress_init(&d, NULL, 0);

    d.source = gzBuf;
    d.source_limit = gzBuf + gzLen;
    d.source_read_cb = NULL;

    int res = uzlib_gzip_parse_header(&d);
    Serial.printf("gzip header parse: %d\n", res);

    d.dest_start = (uint8_t*)jsonBuf;
    d.dest = (uint8_t*)jsonBuf;
    d.dest_limit = (uint8_t*)jsonBuf + sizeof(jsonBuf) - 1;

    res = uzlib_uncompress_chksum(&d);
    int jsonLen = (uint8_t*)d.dest - (uint8_t*)jsonBuf;
    jsonBuf[jsonLen] = '\0';

    Serial.printf("解压结果: %d, JSON长度: %d\n", res, jsonLen);
    Serial.println("=== JSON ===");
    Serial.println(jsonBuf);
    Serial.println("=== END ===");

    if (jsonLen > 10) {
      DynamicJsonDocument doc(1024);
      DeserializationError error = deserializeJson(doc, jsonBuf);

      if (!error) {
        const char* code = doc["code"];
        Serial.printf("API code: %s\n", code ? code : "null");

        if (String(code) == "200") {
          JsonObject now = doc["now"];
          temperature     = String(now["temp"].as<const char*>()).toFloat();
          humidity        = String(now["humidity"].as<const char*>()).toFloat();
          windSpeed       = String(now["windSpeed"].as<const char*>()).toFloat();
          strlcpy(windDir,     now["windDir"]   | "", sizeof(windDir));
          strlcpy(windScale,   now["windScale"] | "", sizeof(windScale));
          strlcpy(weatherDesc, now["text"]      | "", sizeof(weatherDesc));
          weatherIconCode = String(now["icon"].as<const char*>()).toInt();
          weatherValid    = true;
          weatherFailed   = false;

          Serial.printf("温度: %.0f°C, 湿度: %.0f%%, 风速: %.0fkm/h\n",
                        temperature, humidity, windSpeed);
          Serial.print("天气: ");
          Serial.println(weatherDesc);
          Serial.printf("图标代码: %d\n", weatherIconCode);

          drawWeatherInfo();
        } else {
          Serial.print("API返回错误码: ");
          Serial.println(code);
          weatherFailed = true;
        }
      } else {
        Serial.print("JSON解析失败: ");
        Serial.println(error.c_str());
        weatherFailed = true;
      }
    } else {
      Serial.println("解压后数据为空");
      weatherFailed = true;
    }
  } else {
    Serial.printf("HTTP请求失败, 错误码: %d\n", httpCode);
    if (httpCode < 0) {
      Serial.println("连接失败, 可能原因: DNS解析失败/SSL握手失败/网络超时");
    }
    weatherFailed = true;
  }

  http.end();
  lastWeatherUpdate = millis();
  drawLastUpdate();
  Serial.println("====== 天气获取结束 ======\n");
}

// ============ 绘制天气信息 ============
void drawWeatherInfo() {
  // 清除天气区域 (y: 54 ~ 127)
  tft.fillRect(0, 54, 128, 74, COLOR_BG);

  // 重置 last update 绘制状态, 因为整个区域被清空了
  lastUpdateDrawn = false;

  // 重绘下分割线
  tft.drawFastHLine(4, 78, 120, COLOR_DIVIDER);

  if (!weatherValid) {
    tft.setTextSize(1);
    tft.setTextColor(COLOR_DIVIDER, COLOR_BG);
    tft.setCursor(20, 62);
    tft.print("Loading...");
    return;
  }

  // === 城市名 + 天气图标区 (y: 55-76) ===
  tft.setTextSize(1);
  tft.setTextColor(COLOR_CITY, COLOR_BG);
  tft.setCursor(4, 57);
  tft.print(cityName);

  // 天气描述
  tft.setTextColor(COLOR_WEATHER, COLOR_BG);
  tft.setCursor(4, 68);
  tft.print(getWeatherText(weatherIconCode));

  // 绘制天气小图标
  drawWeatherIcon(100, 58, weatherIconCode);

  // === 温度 (大字体, y: 82) + 湿度风力在右侧 ===
  tft.setTextSize(2);
  tft.setTextColor(COLOR_TEMP, COLOR_BG);
  tft.setCursor(4, 84);
  char tempStr[8];
  if (temperature >= 0 && temperature < 100) {
    dtostrf(temperature, 0, 1, tempStr);
  } else {
    dtostrf(temperature, 0, 0, tempStr);
  }
  tft.print(tempStr);
  // 手绘°符号
  int degX = 4 + strlen(tempStr) * 12 + 1;
  int degY = 84;
  tft.drawPixel(degX + 1, degY, COLOR_TEMP);
  tft.drawPixel(degX + 2, degY, COLOR_TEMP);
  tft.drawPixel(degX, degY + 1, COLOR_TEMP);
  tft.drawPixel(degX + 3, degY + 1, COLOR_TEMP);
  tft.drawPixel(degX, degY + 2, COLOR_TEMP);
  tft.drawPixel(degX + 3, degY + 2, COLOR_TEMP);
  tft.drawPixel(degX + 1, degY + 3, COLOR_TEMP);
  tft.drawPixel(degX + 2, degY + 3, COLOR_TEMP);
  tft.setTextSize(1);
  tft.setCursor(degX + 5, degY);
  tft.print("C");

  // === 湿度 + 风力 在温度右侧 (小字体) ===
  tft.setTextSize(1);

  // 湿度 (温度右侧上方)
  tft.setTextColor(COLOR_HUMIDITY, COLOR_BG);
  tft.setCursor(74, 84);
  tft.print("Humi:");
  tft.print((int)humidity);
  tft.print("%");

  // 风力等级 (温度右侧下方)
  tft.setTextColor(COLOR_WIND, COLOR_BG);
  tft.setCursor(74, 96);
  tft.print("Wind:");
  tft.print(windScale);
  // 风向箭头 (在风力数字后面, 与文字垂直居中)
  // "Wind:" = 5字符(30px), 风力数字 1-2位(6-12px), 留2px间距
  // 文字y=96, 高8px, 居中y=100
  int arrowX = 74 + 30 + strlen(windScale) * 6 + 4;
  drawWindArrow(arrowX, 100, windDir);

  // === last update 显示 ===
  drawLastUpdate();
}

// ============ 绘制 last update 状态 ============
// 固定格式: "updated: XXmin ago" 或 "updated: XXmin fail"
// 更新中显示: "updating..."
// 只在状态切换时重绘整行, 平时只更新数字部分

void drawUpdating() {
  tft.fillRect(0, 116, 128, 12, COLOR_BG);
  tft.setTextSize(1);
  tft.setTextColor(COLOR_TIME, COLOR_BG);
  tft.setCursor(31, 118);
  tft.print("updating...");
  // 重置绘制状态, 更新完成后会重绘整行
  lastUpdateDrawn = false;
}

void drawLastUpdate() {
  if (lastWeatherUpdate == 0) return;

  unsigned long elapsed = (millis() - lastWeatherUpdate) / 60000;
  if (elapsed > 99) elapsed = 99;  // 最多显示两位
  int minVal = (int)elapsed;

  tft.setTextSize(1);

  // 如果失败状态发生变化, 或者从未绘制过, 重绘整行
  if (!lastUpdateDrawn || weatherFailed != lastUpdateFailed) {
    tft.fillRect(0, 116, 128, 12, COLOR_BG);

    if (weatherFailed) {
      // "updated:XXmin fail" 18字符, cx=10
      tft.setTextColor(COLOR_UPDATE, COLOR_BG);
      tft.setCursor(10, 118);
      tft.print("updated:");
      // 数字位置: 10 + 8*6 = 58
      char numBuf[3];
      sprintf(numBuf, "%2d", minVal);
      tft.print(numBuf);
      tft.print("min ");
      tft.setTextColor(ST7735_RED, COLOR_BG);
      tft.print("fail");
    } else {
      // "updated:XXmin ago" 17字符, cx=13
      tft.setTextColor(COLOR_UPDATE, COLOR_BG);
      tft.setCursor(13, 118);
      tft.print("updated:");
      // 数字位置: 13 + 8*6 = 61
      char numBuf[3];
      sprintf(numBuf, "%2d", minVal);
      tft.print(numBuf);
      tft.print("min ago");
    }

    lastUpdateDrawn = true;
    lastUpdateFailed = weatherFailed;
    lastUpdateMinVal = minVal;
    return;
  }

  // 只更新数字部分 (两个字符宽度 = 12px)
  if (minVal != lastUpdateMinVal) {
    // 数字起始x: failed=58, normal=61
    int numX = weatherFailed ? 58 : 61;
    uint16_t color = weatherFailed ? ST7735_RED : COLOR_UPDATE;

    // 擦除旧数字
    tft.fillRect(numX, 118, 12, 8, COLOR_BG);
    // 绘制新数字
    tft.setTextColor(color, COLOR_BG);
    tft.setCursor(numX, 118);
    char numBuf[3];
    sprintf(numBuf, "%2d", minVal);
    tft.print(numBuf);

    lastUpdateMinVal = minVal;
  }
}

// ============ 天气图标绘制 (和风天气图标代码) ============
void drawWeatherIcon(int x, int y, int icon) {
  // 和风天气图标代码:
  // 100/150=晴, 101-103/151-153=云, 104=阴
  // 300-318/350-351=雨, 400-410/456-457=雪, 500-515=雾霾

  if (icon == 100 || icon == 150) {
    // 晴 
    tft.drawPixel(x + 8, y + 0, 0xFFE0);
    tft.drawPixel(x + 9, y + 0, 0xFFE0);
    tft.drawPixel(x + 1, y + 1, 0xFFE0);
    tft.drawPixel(x + 2, y + 1, 0xFFE0);
    tft.drawPixel(x + 8, y + 1, 0xFFE0);
    tft.drawPixel(x + 9, y + 1, 0xFFE0);
    tft.drawPixel(x + 15, y + 1, 0xFFE0);
    tft.drawPixel(x + 16, y + 1, 0xFFE0);
    tft.drawFastHLine(x + 1, y + 2, 3, 0xFFE0);
    tft.drawPixel(x + 8, y + 2, 0xFFE0);
    tft.drawPixel(x + 9, y + 2, 0xFFE0);
    tft.drawFastHLine(x + 14, y + 2, 3, 0xFFE0);
    tft.drawFastHLine(x + 2, y + 3, 3, 0xFFE0);
    tft.drawFastHLine(x + 13, y + 3, 3, 0xFFE0);
    tft.drawPixel(x + 3, y + 4, 0xFFE0);
    tft.drawPixel(x + 4, y + 4, 0xFFE0);
    tft.drawFastHLine(x + 6, y + 4, 6, 0xFFE0);
    tft.drawPixel(x + 13, y + 4, 0xFFE0);
    tft.drawPixel(x + 14, y + 4, 0xFFE0);
    tft.drawFastHLine(x + 5, y + 5, 8, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 6, 10, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 7, 10, 0xFFE0);
    tft.drawFastHLine(x + 0, y + 8, 3, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 8, 10, 0xFFE0);
    tft.drawFastHLine(x + 15, y + 8, 3, 0xFFE0);
    tft.drawFastHLine(x + 0, y + 9, 3, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 9, 10, 0xFFE0);
    tft.drawFastHLine(x + 15, y + 9, 3, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 10, 10, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 11, 10, 0xFFE0);
    tft.drawFastHLine(x + 5, y + 12, 8, 0xFFE0);
    tft.drawPixel(x + 3, y + 13, 0xFFE0);
    tft.drawPixel(x + 4, y + 13, 0xFFE0);
    tft.drawFastHLine(x + 6, y + 13, 6, 0xFFE0);
    tft.drawPixel(x + 13, y + 13, 0xFFE0);
    tft.drawPixel(x + 14, y + 13, 0xFFE0);
    tft.drawFastHLine(x + 2, y + 14, 3, 0xFFE0);
    tft.drawFastHLine(x + 13, y + 14, 3, 0xFFE0);
    tft.drawFastHLine(x + 1, y + 15, 3, 0xFFE0);
    tft.drawPixel(x + 8, y + 15, 0xFFE0);
    tft.drawPixel(x + 9, y + 15, 0xFFE0);
    tft.drawFastHLine(x + 14, y + 15, 3, 0xFFE0);
    tft.drawPixel(x + 1, y + 16, 0xFFE0);
    tft.drawPixel(x + 2, y + 16, 0xFFE0);
    tft.drawPixel(x + 8, y + 16, 0xFFE0);
    tft.drawPixel(x + 9, y + 16, 0xFFE0);
    tft.drawPixel(x + 15, y + 16, 0xFFE0);
    tft.drawPixel(x + 16, y + 16, 0xFFE0);
    tft.drawPixel(x + 8, y + 17, 0xFFE0);
    tft.drawPixel(x + 9, y + 17, 0xFFE0);
  } else if ((icon >= 101 && icon <= 103)) {
    // 日间多云/少云 
    tft.drawFastHLine(x + 2, y + 2, 4, 0xFFE0);
    tft.drawFastHLine(x + 1, y + 3, 6, 0xFFE0);
    tft.drawFastHLine(x + 0, y + 4, 7, 0xFFE0);
    tft.drawFastHLine(x + 0, y + 5, 6, 0xFFE0);
    tft.drawFastHLine(x + 0, y + 6, 4, 0xFFE0);
    tft.drawPixel(x + 0, y + 7, 0xFFE0);
    tft.drawPixel(x + 8, y + 3, 0xB5F6);
    tft.drawPixel(x + 9, y + 3, 0xB5F6);
    tft.drawFastHLine(x + 7, y + 4, 5, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 5, 7, 0xB5F6);
    tft.drawFastHLine(x + 4, y + 6, 12, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 7, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 8, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 9, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 10, 11, 0xB5F6);
    tft.drawFastHLine(x + 14, y + 10, 4, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 11, 7, 0xB5F6);
    tft.drawPixel(x + 16, y + 11, 0xB5F6);
    tft.drawPixel(x + 17, y + 11, 0xB5F6);
    tft.drawFastHLine(x + 11, y + 10, 3, 0x4208);
    tft.drawFastHLine(x + 8, y + 11, 8, 0x4208);
    tft.drawFastHLine(x + 5, y + 12, 13, 0x4208);
    tft.drawFastHLine(x + 5, y + 13, 13, 0x4208);
    tft.drawFastHLine(x + 7, y + 14, 10, 0x4208);
    tft.drawFastHLine(x + 9, y + 15, 7, 0x4208);
  } else if ((icon >= 151 && icon <= 153)) {
    // 夜间多云/少云 
    tft.drawPixel(x + 12, y + 1, 0xFFFF);
    tft.drawPixel(x + 13, y + 1, 0xFFFF);
    tft.drawPixel(x + 13, y + 2, 0xFFFF);
    tft.drawPixel(x + 14, y + 2, 0xFFFF);
    tft.drawPixel(x + 13, y + 3, 0xFFFF);
    tft.drawPixel(x + 14, y + 3, 0xFFFF);
    tft.drawFastHLine(x + 12, y + 4, 3, 0xFFFF);
    tft.drawFastHLine(x + 11, y + 5, 3, 0xFFFF);
    tft.drawFastHLine(x + 5, y + 3, 3, 0xB5F6);
    tft.drawFastHLine(x + 4, y + 4, 5, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 5, 9, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 6, 13, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 17, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 8, 17, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 9, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 10, 11, 0xB5F6);
    tft.drawFastHLine(x + 14, y + 10, 4, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 11, 7, 0xB5F6);
    tft.drawPixel(x + 16, y + 11, 0xB5F6);
    tft.drawPixel(x + 17, y + 11, 0xB5F6);
    tft.drawFastHLine(x + 11, y + 10, 3, 0x4208);
    tft.drawFastHLine(x + 8, y + 11, 8, 0x4208);
    tft.drawFastHLine(x + 5, y + 12, 13, 0x4208);
    tft.drawFastHLine(x + 5, y + 13, 13, 0x4208);
    tft.drawFastHLine(x + 7, y + 14, 10, 0x4208);
    tft.drawFastHLine(x + 9, y + 15, 7, 0x4208);
  } else if (icon == 104) {
    // 阴天 
    tft.drawPixel(x + 8, y + 3, 0xB5F6);
    tft.drawPixel(x + 9, y + 3, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 4, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 5, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 6, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 7, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 8, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 9, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 10, 18, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 11, 16, 0xB5F6);
    tft.drawFastHLine(x + 4, y + 12, 10, 0xB5F6);
  } else if (icon == 300 || icon == 301 || icon == 305 || icon == 309 || icon == 350 || icon == 351) {
    // 阵雨/小雨/毛毛雨
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 9, y + 10, 0x05FF);
    tft.drawPixel(x + 8, y + 11, 0x05FF);
    tft.drawPixel(x + 9, y + 11, 0x05FF);
    tft.drawPixel(x + 4, y + 12, 0x05FF);
    tft.drawPixel(x + 8, y + 12, 0x05FF);
    tft.drawPixel(x + 14, y + 12, 0x05FF);
    tft.drawPixel(x + 3, y + 13, 0x05FF);
    tft.drawPixel(x + 4, y + 13, 0x05FF);
    tft.drawPixel(x + 13, y + 13, 0x05FF);
    tft.drawPixel(x + 14, y + 13, 0x05FF);
    tft.drawPixel(x + 3, y + 14, 0x05FF);
    tft.drawPixel(x + 13, y + 14, 0x05FF);
  } else if (icon == 302 || icon == 303 || icon == 304) {
    // 雷阵雨/强雷阵雨/雷阵雨伴冰雹
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 10, y + 9, 0xFFE0);
    tft.drawPixel(x + 9, y + 10, 0xFFE0);
    tft.drawPixel(x + 8, y + 11, 0xFFE0);
    tft.drawPixel(x + 7, y + 12, 0xFFE0);
    tft.drawFastHLine(x + 6, y + 13, 5, 0xFFE0);
    tft.drawPixel(x + 9, y + 14, 0xFFE0);
    tft.drawPixel(x + 8, y + 15, 0xFFE0);
    tft.drawPixel(x + 7, y + 16, 0xFFE0);
    tft.drawPixel(x + 6, y + 17, 0xFFE0);
    tft.drawPixel(x + 4, y + 10, 0x05FF);
    tft.drawPixel(x + 15, y + 10, 0x05FF);
    tft.drawPixel(x + 3, y + 11, 0x05FF);
    tft.drawPixel(x + 14, y + 11, 0x05FF);
    tft.drawPixel(x + 15, y + 11, 0x05FF);
    tft.drawPixel(x + 2, y + 12, 0x05FF);
    tft.drawPixel(x + 3, y + 12, 0x05FF);
    tft.drawPixel(x + 13, y + 12, 0x05FF);
    tft.drawPixel(x + 14, y + 12, 0x05FF);
    tft.drawPixel(x + 2, y + 13, 0x05FF);
    tft.drawPixel(x + 13, y + 13, 0x05FF);
  } else if (icon == 306 || icon == 314 || icon == 315 || icon == 399) {
    // 中雨/小到中雨/中到大雨
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 17, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 5, y + 10, 0x05FF);
    tft.drawPixel(x + 9, y + 10, 0x05FF);
    tft.drawPixel(x + 13, y + 10, 0x05FF);
    tft.drawPixel(x + 4, y + 11, 0x05FF);
    tft.drawPixel(x + 5, y + 11, 0x05FF);
    tft.drawPixel(x + 8, y + 11, 0x05FF);
    tft.drawPixel(x + 9, y + 11, 0x05FF);
    tft.drawPixel(x + 12, y + 11, 0x05FF);
    tft.drawPixel(x + 13, y + 11, 0x05FF);
    tft.drawPixel(x + 4, y + 12, 0x05FF);
    tft.drawPixel(x + 8, y + 12, 0x05FF);
    tft.drawPixel(x + 12, y + 12, 0x05FF);
    tft.drawPixel(x + 3, y + 13, 0x05FF);
    tft.drawPixel(x + 4, y + 13, 0x05FF);
    tft.drawPixel(x + 7, y + 13, 0x05FF);
    tft.drawPixel(x + 8, y + 13, 0x05FF);
    tft.drawPixel(x + 11, y + 13, 0x05FF);
    tft.drawPixel(x + 12, y + 13, 0x05FF);
    tft.drawPixel(x + 3, y + 14, 0x05FF);
    tft.drawPixel(x + 7, y + 14, 0x05FF);
    tft.drawPixel(x + 11, y + 14, 0x05FF);
    tft.drawPixel(x + 7, y + 15, 0x05FF);
  } else if ((icon >= 307 && icon <= 312) || icon == 316 || icon == 317 || icon == 318) {
    // 大雨/暴雨/大到暴雨/暴雨到大暴雨/大暴雨到特大暴雨
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 17, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 2, y + 10, 0x05FF);
    tft.drawPixel(x + 6, y + 10, 0x05FF);
    tft.drawPixel(x + 9, y + 10, 0x05FF);
    tft.drawPixel(x + 13, y + 10, 0x05FF);
    tft.drawPixel(x + 16, y + 10, 0x05FF);
    tft.drawPixel(x + 1, y + 11, 0x05FF);
    tft.drawPixel(x + 2, y + 11, 0x05FF);
    tft.drawPixel(x + 5, y + 11, 0x05FF);
    tft.drawPixel(x + 6, y + 11, 0x05FF);
    tft.drawPixel(x + 8, y + 11, 0x05FF);
    tft.drawPixel(x + 9, y + 11, 0x05FF);
    tft.drawPixel(x + 12, y + 11, 0x05FF);
    tft.drawPixel(x + 13, y + 11, 0x05FF);
    tft.drawPixel(x + 15, y + 11, 0x05FF);
    tft.drawPixel(x + 16, y + 11, 0x05FF);
    tft.drawPixel(x + 0, y + 12, 0x05FF);
    tft.drawPixel(x + 1, y + 12, 0x05FF);
    tft.drawPixel(x + 4, y + 12, 0x05FF);
    tft.drawPixel(x + 5, y + 12, 0x05FF);
    tft.drawPixel(x + 8, y + 12, 0x05FF);
    tft.drawPixel(x + 12, y + 12, 0x05FF);
    tft.drawPixel(x + 15, y + 12, 0x05FF);
    tft.drawPixel(x + 0, y + 13, 0x05FF);
    tft.drawPixel(x + 3, y + 13, 0x05FF);
    tft.drawPixel(x + 4, y + 13, 0x05FF);
    tft.drawPixel(x + 7, y + 13, 0x05FF);
    tft.drawPixel(x + 8, y + 13, 0x05FF);
    tft.drawPixel(x + 11, y + 13, 0x05FF);
    tft.drawPixel(x + 12, y + 13, 0x05FF);
    tft.drawPixel(x + 14, y + 13, 0x05FF);
    tft.drawPixel(x + 15, y + 13, 0x05FF);
    tft.drawPixel(x + 2, y + 14, 0x05FF);
    tft.drawPixel(x + 3, y + 14, 0x05FF);
    tft.drawPixel(x + 7, y + 14, 0x05FF);
    tft.drawPixel(x + 10, y + 14, 0x05FF);
    tft.drawPixel(x + 11, y + 14, 0x05FF);
    tft.drawPixel(x + 14, y + 14, 0x05FF);
    tft.drawPixel(x + 2, y + 15, 0x05FF);
    tft.drawPixel(x + 6, y + 15, 0x05FF);
    tft.drawPixel(x + 7, y + 15, 0x05FF);
    tft.drawPixel(x + 10, y + 15, 0x05FF);
    tft.drawPixel(x + 5, y + 16, 0x05FF);
    tft.drawPixel(x + 6, y + 16, 0x05FF);
    tft.drawPixel(x + 9, y + 16, 0x05FF);
    tft.drawPixel(x + 10, y + 16, 0x05FF);
    tft.drawPixel(x + 5, y + 17, 0x05FF);
  } else if (icon == 313) {
    // 冻雨
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 3, y + 10, 0x05FF);
    tft.drawPixel(x + 14, y + 10, 0x05FF);
    tft.drawPixel(x + 1, y + 11, 0x05FF);
    tft.drawPixel(x + 3, y + 11, 0x05FF);
    tft.drawPixel(x + 5, y + 11, 0x05FF);
    tft.drawPixel(x + 13, y + 11, 0x05FF);
    tft.drawPixel(x + 14, y + 11, 0x05FF);
    tft.drawFastHLine(x + 2, y + 12, 3, 0x05FF);
    tft.drawPixel(x + 13, y + 12, 0x05FF);
    tft.drawFastHLine(x + 0, y + 13, 7, 0x05FF);
    tft.drawPixel(x + 11, y + 13, 0x05FF);
    tft.drawPixel(x + 16, y + 13, 0x05FF);
    tft.drawFastHLine(x + 2, y + 14, 3, 0x05FF);
    tft.drawPixel(x + 10, y + 14, 0x05FF);
    tft.drawPixel(x + 11, y + 14, 0x05FF);
    tft.drawPixel(x + 15, y + 14, 0x05FF);
    tft.drawPixel(x + 16, y + 14, 0x05FF);
    tft.drawPixel(x + 1, y + 15, 0x05FF);
    tft.drawPixel(x + 3, y + 15, 0x05FF);
    tft.drawPixel(x + 5, y + 15, 0x05FF);
    tft.drawPixel(x + 10, y + 15, 0x05FF);
    tft.drawPixel(x + 15, y + 15, 0x05FF);
    tft.drawPixel(x + 3, y + 16, 0x05FF);
  } else if (icon == 400) {
    // 小雪 
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 3, y + 10, 0xFFFF);
    tft.drawFastHLine(x + 2, y + 11, 3, 0xFFFF);
    tft.drawPixel(x + 3, y + 12, 0xFFFF);
    tft.drawPixel(x + 14, y + 10, 0xFFFF);
    tft.drawFastHLine(x + 13, y + 11, 3, 0xFFFF);
    tft.drawPixel(x + 14, y + 12, 0xFFFF);
    tft.drawPixel(x + 9, y + 11, 0xFFFF);
    tft.drawFastHLine(x + 8, y + 12, 3, 0xFFFF);
    tft.drawPixel(x + 9, y + 13, 0xFFFF);
  } else if (icon == 401 || icon == 408) {
    // 中雪/小到中雪 
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 2, y + 10, 0xFFFF);
    tft.drawFastHLine(x + 1, y + 11, 3, 0xFFFF);
    tft.drawPixel(x + 2, y + 12, 0xFFFF);
    tft.drawPixel(x + 15, y + 10, 0xFFFF);
    tft.drawFastHLine(x + 14, y + 11, 3, 0xFFFF);
    tft.drawPixel(x + 15, y + 12, 0xFFFF);
    tft.drawPixel(x + 6, y + 12, 0xFFFF);
    tft.drawFastHLine(x + 5, y + 13, 3, 0xFFFF);
    tft.drawPixel(x + 6, y + 14, 0xFFFF);
    tft.drawPixel(x + 11, y + 12, 0xFFFF);
    tft.drawFastHLine(x + 10, y + 13, 3, 0xFFFF);
    tft.drawPixel(x + 11, y + 14, 0xFFFF);
  } else if (icon == 402 || icon == 403 || icon == 409 || icon == 410 || icon == 499) {
    // 大雪/暴雪/中到大雪/大到暴雪
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 2, y + 10, 0xFFFF);
    tft.drawPixel(x + 14, y + 10, 0xFFFF);
    tft.drawFastHLine(x + 1, y + 11, 3, 0xFFFF);
    tft.drawPixel(x + 8, y + 11, 0xFFFF);
    tft.drawFastHLine(x + 13, y + 11, 3, 0xFFFF);
    tft.drawFastHLine(x + 0, y + 12, 5, 0xFFFF);
    tft.drawPixel(x + 6, y + 12, 0xFFFF);
    tft.drawPixel(x + 8, y + 12, 0xFFFF);
    tft.drawPixel(x + 10, y + 12, 0xFFFF);
    tft.drawFastHLine(x + 12, y + 12, 5, 0xFFFF);
    tft.drawFastHLine(x + 1, y + 13, 3, 0xFFFF);
    tft.drawFastHLine(x + 7, y + 13, 3, 0xFFFF);
    tft.drawFastHLine(x + 13, y + 13, 3, 0xFFFF);
    tft.drawPixel(x + 2, y + 14, 0xFFFF);
    tft.drawFastHLine(x + 5, y + 14, 7, 0xFFFF);
    tft.drawPixel(x + 14, y + 14, 0xFFFF);
    tft.drawFastHLine(x + 7, y + 15, 3, 0xFFFF);
    tft.drawPixel(x + 6, y + 16, 0xFFFF);
    tft.drawPixel(x + 8, y + 16, 0xFFFF);
    tft.drawPixel(x + 10, y + 16, 0xFFFF);
    tft.drawPixel(x + 8, y + 17, 0xFFFF);
  } else if (icon == 404 || icon == 405 || icon == 406 || icon == 407 || icon == 456 || icon == 457) {
    // 雨夹雪/阵雨夹雪/阵雪 
    tft.drawPixel(x + 8, y + 0, 0xB5F6);
    tft.drawPixel(x + 9, y + 0, 0xB5F6);
    tft.drawFastHLine(x + 6, y + 1, 6, 0xB5F6);
    tft.drawFastHLine(x + 5, y + 2, 8, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 3, 14, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 4, 16, 0xB5F6);
    tft.drawFastHLine(x + 1, y + 5, 16, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 6, 18, 0xB5F6);
    tft.drawFastHLine(x + 0, y + 7, 18, 0xB5F6);
    tft.drawFastHLine(x + 2, y + 8, 14, 0xB5F6);
    tft.drawPixel(x + 2, y + 10, 0xFFFF);
    tft.drawFastHLine(x + 1, y + 11, 3, 0xFFFF);
    tft.drawPixel(x + 2, y + 12, 0xFFFF);
    tft.drawPixel(x + 5, y + 13, 0xFFFF);
    tft.drawFastHLine(x + 4, y + 14, 3, 0xFFFF);
    tft.drawPixel(x + 5, y + 15, 0xFFFF);
    tft.drawPixel(x + 10, y + 10, 0x05FF);
    tft.drawPixel(x + 15, y + 10, 0x05FF);
    tft.drawPixel(x + 9, y + 11, 0x05FF);
    tft.drawPixel(x + 10, y + 11, 0x05FF);
    tft.drawPixel(x + 14, y + 11, 0x05FF);
    tft.drawPixel(x + 15, y + 11, 0x05FF);
    tft.drawPixel(x + 9, y + 12, 0x05FF);
    tft.drawPixel(x + 14, y + 12, 0x05FF);
    tft.drawPixel(x + 12, y + 13, 0x05FF);
    tft.drawPixel(x + 11, y + 14, 0x05FF);
    tft.drawPixel(x + 12, y + 14, 0x05FF);
    tft.drawPixel(x + 11, y + 15, 0x05FF);
  } else if (icon == 500 || icon == 501 || icon == 509 || icon == 510 || icon == 514 || icon == 515) {
    // 雾 (Fog/Mist) 
    tft.drawFastHLine(x + 2, y + 5, 3, 0xFFFF);
    tft.drawFastHLine(x + 13, y + 5, 3, 0xFFFF);
    tft.drawPixel(x + 1, y + 6, 0xFFFF);
    tft.drawPixel(x + 5, y + 6, 0xFFFF);
    tft.drawPixel(x + 6, y + 6, 0xFFFF);
    tft.drawPixel(x + 11, y + 6, 0xFFFF);
    tft.drawPixel(x + 12, y + 6, 0xFFFF);
    tft.drawPixel(x + 16, y + 6, 0xFFFF);
    tft.drawPixel(x + 0, y + 7, 0xFFFF);
    tft.drawPixel(x + 7, y + 7, 0xFFFF);
    tft.drawPixel(x + 10, y + 7, 0xFFFF);
    tft.drawPixel(x + 17, y + 7, 0xFFFF);
    tft.drawPixel(x + 0, y + 8, 0xFFFF);
    tft.drawPixel(x + 8, y + 8, 0xFFFF);
    tft.drawPixel(x + 9, y + 8, 0xFFFF);
    tft.drawPixel(x + 17, y + 8, 0xFFFF);
    tft.drawPixel(x + 0, y + 9, 0xFFFF);
    tft.drawPixel(x + 8, y + 9, 0xFFFF);
    tft.drawPixel(x + 9, y + 9, 0xFFFF);
    tft.drawPixel(x + 17, y + 9, 0xFFFF);
    tft.drawPixel(x + 0, y + 10, 0xFFFF);
    tft.drawPixel(x + 7, y + 10, 0xFFFF);
    tft.drawPixel(x + 10, y + 10, 0xFFFF);
    tft.drawPixel(x + 17, y + 10, 0xFFFF);
    tft.drawPixel(x + 1, y + 11, 0xFFFF);
    tft.drawPixel(x + 5, y + 11, 0xFFFF);
    tft.drawPixel(x + 6, y + 11, 0xFFFF);
    tft.drawPixel(x + 11, y + 11, 0xFFFF);
    tft.drawPixel(x + 12, y + 11, 0xFFFF);
    tft.drawPixel(x + 16, y + 11, 0xFFFF);
    tft.drawFastHLine(x + 2, y + 12, 3, 0xFFFF);
    tft.drawFastHLine(x + 13, y + 12, 3, 0xFFFF);
  } else if (icon == 502 || icon == 511 || icon == 512 || icon == 513) {
    // 霾 (Haze) 
    tft.drawPixel(x + 2, y + 1, 0xFD00);
    tft.drawPixel(x + 3, y + 1, 0xFD00);
    tft.drawPixel(x + 8, y + 1, 0xFD00);
    tft.drawPixel(x + 9, y + 1, 0xFD00);
    tft.drawPixel(x + 14, y + 1, 0xFD00);
    tft.drawPixel(x + 15, y + 1, 0xFD00);
    tft.drawPixel(x + 2, y + 2, 0xFD00);
    tft.drawPixel(x + 3, y + 2, 0xFD00);
    tft.drawPixel(x + 8, y + 2, 0xFD00);
    tft.drawPixel(x + 9, y + 2, 0xFD00);
    tft.drawPixel(x + 14, y + 2, 0xFD00);
    tft.drawPixel(x + 15, y + 2, 0xFD00);
    tft.drawFastHLine(x + 2, y + 5, 3, 0xFD00);
    tft.drawFastHLine(x + 13, y + 5, 3, 0xFD00);
    tft.drawPixel(x + 1, y + 6, 0xFD00);
    tft.drawPixel(x + 5, y + 6, 0xFD00);
    tft.drawPixel(x + 6, y + 6, 0xFD00);
    tft.drawPixel(x + 11, y + 6, 0xFD00);
    tft.drawPixel(x + 12, y + 6, 0xFD00);
    tft.drawPixel(x + 16, y + 6, 0xFD00);
    tft.drawPixel(x + 0, y + 7, 0xFD00);
    tft.drawPixel(x + 7, y + 7, 0xFD00);
    tft.drawPixel(x + 10, y + 7, 0xFD00);
    tft.drawPixel(x + 17, y + 7, 0xFD00);
    tft.drawPixel(x + 0, y + 8, 0xFD00);
    tft.drawPixel(x + 8, y + 8, 0xFD00);
    tft.drawPixel(x + 9, y + 8, 0xFD00);
    tft.drawPixel(x + 17, y + 8, 0xFD00);
    tft.drawPixel(x + 0, y + 9, 0xFD00);
    tft.drawPixel(x + 8, y + 9, 0xFD00);
    tft.drawPixel(x + 9, y + 9, 0xFD00);
    tft.drawPixel(x + 17, y + 9, 0xFD00);
    tft.drawPixel(x + 0, y + 10, 0xFD00);
    tft.drawPixel(x + 7, y + 10, 0xFD00);
    tft.drawPixel(x + 10, y + 10, 0xFD00);
    tft.drawPixel(x + 17, y + 10, 0xFD00);
    tft.drawPixel(x + 1, y + 11, 0xFD00);
    tft.drawPixel(x + 5, y + 11, 0xFD00);
    tft.drawPixel(x + 6, y + 11, 0xFD00);
    tft.drawPixel(x + 11, y + 11, 0xFD00);
    tft.drawPixel(x + 12, y + 11, 0xFD00);
    tft.drawPixel(x + 16, y + 11, 0xFD00);
    tft.drawFastHLine(x + 2, y + 12, 3, 0xFD00);
    tft.drawFastHLine(x + 13, y + 12, 3, 0xFD00);
    tft.drawPixel(x + 2, y + 15, 0xFD00);
    tft.drawPixel(x + 3, y + 15, 0xFD00);
    tft.drawPixel(x + 8, y + 15, 0xFD00);
    tft.drawPixel(x + 9, y + 15, 0xFD00);
    tft.drawPixel(x + 14, y + 15, 0xFD00);
    tft.drawPixel(x + 15, y + 15, 0xFD00);
    tft.drawPixel(x + 2, y + 16, 0xFD00);
    tft.drawPixel(x + 3, y + 16, 0xFD00);
    tft.drawPixel(x + 8, y + 16, 0xFD00);
    tft.drawPixel(x + 9, y + 16, 0xFD00);
    tft.drawPixel(x + 14, y + 16, 0xFD00);
    tft.drawPixel(x + 15, y + 16, 0xFD00);
  } else if (icon == 503 || icon == 504) {
    // 扬沙/浮尘 
    tft.drawFastHLine(x + 9, y + 1, 4, 0xFD20);
    tft.drawFastHLine(x + 10, y + 2, 5, 0xFD20);
    tft.drawPixel(x + 1, y + 3, 0xFD20);
    tft.drawPixel(x + 2, y + 3, 0xFD20);
    tft.drawFastHLine(x + 11, y + 3, 5, 0xFD20);
    tft.drawPixel(x + 1, y + 4, 0xFD20);
    tft.drawPixel(x + 2, y + 4, 0xFD20);
    tft.drawPixel(x + 6, y + 4, 0xFD20);
    tft.drawPixel(x + 7, y + 4, 0xFD20);
    tft.drawFastHLine(x + 12, y + 4, 5, 0xFD20);
    tft.drawPixel(x + 6, y + 5, 0xFD20);
    tft.drawPixel(x + 7, y + 5, 0xFD20);
    tft.drawFastHLine(x + 12, y + 5, 6, 0xFD20);
    tft.drawFastHLine(x + 13, y + 6, 5, 0xFD20);
    tft.drawPixel(x + 3, y + 7, 0xFD20);
    tft.drawPixel(x + 4, y + 7, 0xFD20);
    tft.drawFastHLine(x + 13, y + 7, 5, 0xFD20);
    tft.drawPixel(x + 3, y + 8, 0xFD20);
    tft.drawPixel(x + 4, y + 8, 0xFD20);
    tft.drawFastHLine(x + 13, y + 8, 5, 0xFD20);
    tft.drawPixel(x + 8, y + 9, 0xFD20);
    tft.drawFastHLine(x + 11, y + 9, 7, 0xFD20);
    tft.drawFastHLine(x + 10, y + 10, 8, 0xFD20);
    tft.drawPixel(x + 5, y + 11, 0xFD20);
    tft.drawFastHLine(x + 10, y + 11, 7, 0xFD20);
    tft.drawPixel(x + 1, y + 12, 0xFD20);
    tft.drawFastHLine(x + 8, y + 12, 8, 0xFD20);
    tft.drawFastHLine(x + 8, y + 13, 7, 0xFD20);
    tft.drawFastHLine(x + 5, y + 14, 9, 0xFD20);
    tft.drawFastHLine(x + 2, y + 15, 10, 0xFD20);
    tft.drawFastHLine(x + 3, y + 16, 6, 0xFD20);
  } else if (icon == 507 || icon == 508) {
    // 沙尘暴/强沙尘暴 
    tft.drawFastHLine(x + 6, y + 0, 5, 0xFFE0);
    tft.drawFastHLine(x + 5, y + 1, 8, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 2, 3, 0xFFE0);
    tft.drawFastHLine(x + 11, y + 2, 3, 0xFFE0);
    tft.drawPixel(x + 3, y + 3, 0xFFE0);
    tft.drawPixel(x + 4, y + 3, 0xFFE0);
    tft.drawPixel(x + 12, y + 3, 0xFFE0);
    tft.drawPixel(x + 13, y + 3, 0xFFE0);
    tft.drawPixel(x + 3, y + 4, 0xFFE0);
    tft.drawPixel(x + 4, y + 4, 0xFFE0);
    tft.drawPixel(x + 3, y + 5, 0xFFE0);
    tft.drawPixel(x + 4, y + 5, 0xFFE0);
    tft.drawPixel(x + 4, y + 6, 0xFFE0);
    tft.drawPixel(x + 5, y + 6, 0xFFE0);
    tft.drawFastHLine(x + 5, y + 7, 3, 0xFFE0);
    tft.drawFastHLine(x + 1, y + 8, 16, 0xFFFF);
    tft.drawPixel(x + 15, y + 7, 0xFFFF);
    tft.drawPixel(x + 1, y + 9, 0xFFFF);
    tft.drawPixel(x + 16, y + 9, 0xFFFF);
    tft.drawPixel(x + 17, y + 9, 0xFFFF);
    tft.drawFastHLine(x + 1, y + 10, 16, 0xFFFF);
    tft.drawPixel(x + 15, y + 11, 0xFFFF);
    tft.drawFastHLine(x + 10, y + 11, 3, 0xFFE0);
    tft.drawFastHLine(x + 11, y + 12, 3, 0xFFE0);
    tft.drawPixel(x + 2, y + 13, 0xFFE0);
    tft.drawPixel(x + 3, y + 13, 0xFFE0);
    tft.drawPixel(x + 12, y + 13, 0xFFE0);
    tft.drawPixel(x + 13, y + 13, 0xFFE0);
    tft.drawFastHLine(x + 2, y + 14, 3, 0xFFE0);
    tft.drawPixel(x + 12, y + 14, 0xFFE0);
    tft.drawPixel(x + 13, y + 14, 0xFFE0);
    tft.drawFastHLine(x + 3, y + 15, 3, 0xFFE0);
    tft.drawFastHLine(x + 11, y + 15, 3, 0xFFE0);
    tft.drawFastHLine(x + 4, y + 16, 9, 0xFFE0);
    tft.drawFastHLine(x + 6, y + 17, 5, 0xFFE0);
  } else {
    // 未知天气
    tft.setTextSize(2);
    tft.setTextColor(COLOR_DIVIDER, COLOR_BG);
    tft.setCursor(x + 2, y + 2);
    tft.print("?");
    tft.setTextSize(1);
  }
}

// ============ 绘制风向小旗 ============
// 旗杆指向风吹来的方向, 旗帜在杆顶侧面
void drawWindArrow(int cx, int cy, const char* windDir) {
  // 解析方向为dx,dy (-1/0/1), 表示风吹来的方向
  int dx = 0, dy = 0;
  if (strstr(windDir, "\xe4\xb8\x9c")) dx = 1;   // 东
  if (strstr(windDir, "\xe8\xa5\xbf")) dx = -1;  // 西
  if (strstr(windDir, "\xe5\x8c\x97")) dy = -1;  // 北
  if (strstr(windDir, "\xe5\x8d\x97")) dy = 1;   // 南
  if (dx == 0 && dy == 0) return;

  // 旗杆: 从中心往风来的方向延伸
  int baseX = cx - dx * 3;
  int baseY = cy - dy * 3;
  int topX  = cx + dx * 3;
  int topY  = cy + dy * 3;
  tft.drawLine(baseX, baseY, topX, topY, COLOR_WIND);

  // 旗帜: 在杆顶端侧面画一个小三角旗
  if (dy == 0) {
    // 纯东/西: 杆水平, 旗往上飘
    tft.drawLine(topX, topY, topX - dx * 2, topY - 3, COLOR_WIND);
    tft.drawLine(topX - dx * 2, topY - 3, topX - dx * 2, topY, COLOR_WIND);
  } else if (dx == 0) {
    // 纯北/南: 杆竖直, 旗往右飘
    tft.drawLine(topX, topY, topX + 3, topY - dy * 2, COLOR_WIND);
    tft.drawLine(topX + 3, topY - dy * 2, topX, topY - dy * 2, COLOR_WIND);
  } else {
    // 对角: 杆斜向, 旗垂直于杆方向飘
    // 旗帜朝杆的右手侧展开
    tft.drawLine(topX, topY, topX + dy * 3, topY - dx * 3, COLOR_WIND);
    tft.drawLine(topX + dy * 3, topY - dx * 3, topX - dx * 2, topY - dy * 2, COLOR_WIND);
  }
}

// ============ 天气描述文本 (英文) ============
const char* getWeatherText(int icon) {
  if (icon == 100 || icon == 150) return "Clear";
  if (icon == 101 || icon == 151) return "Cloudy";
  if (icon == 102 || icon == 152) return "Few Clouds";
  if (icon == 103 || icon == 153) return "Partly Cloudy";
  if (icon == 104) return "Overcast";
  if (icon == 300 || icon == 350) return "Showers";
  if (icon == 301) return "Light Rain";
  if (icon == 302) return "Thunderstorm";
  if (icon == 303) return "Thundershower";
  if (icon == 304) return "Heavy Rain";
  if (icon == 305) return "Light Rain";
  if (icon == 306) return "Moderate Rain";
  if (icon >= 307 && icon <= 312) return "Rainstorm";
  if (icon >= 313 && icon <= 315) return "Freezing Rain";
  if (icon == 316 || icon == 317) return "Sleet";
  if (icon == 318 || icon == 399) return "Rain";
  if (icon == 400) return "Light Snow";
  if (icon == 401) return "Moderate Snow";
  if (icon == 402) return "Heavy Snow";
  if (icon == 403) return "Blizzard";
  if (icon >= 404 && icon <= 410) return "Sleet/Snow";
  if (icon == 499) return "Snow";
  if (icon == 500) return "Mist";
  if (icon == 501 || icon == 509 || icon == 510) return "Fog";
  if (icon == 502 || icon == 511 || icon == 512) return "Haze";
  if (icon == 503 || icon == 504) return "Sandstorm";
  if (icon == 507 || icon == 508) return "Dust Storm";
  if (icon == 513 || icon == 514 || icon == 515) return "Haze";
  if (icon == 900) return "Hot";
  if (icon == 901) return "Cold";
  return "Unknown";
}
