# TFT Video Player

基于 ESP8266 + ST7735 TFT 屏幕的无线**黑白**视频播放器。通过浏览器选择视频文件，实时串流到 128x128 TFT 屏幕上显示。

> 用来播放Bad apple (doge
>
> https://github.com/user-attachments/assets/3e7bbd97-fd25-4df2-9e3f-f12760cfd587
>
> 当然还能看春晚，当然彩色的话帧率极低，故此不做展示
>
> https://github.com/user-attachments/assets/8d5d36e8-7226-42d5-93ca-e7d79560835d



## 硬件需求

| 组件 | 型号 |
|------|------|
| 主控 | ESP8266 (NodeMCU / D1 Mini 等) |
| 屏幕 | 1.44" ST7735 128x128 TFT |

### 接线

| TFT 引脚 | ESP8266 引脚 |
|-----------|-------------|
| CS | D1 |
| DC | D3 |
| RST | D2 |
| LED | D4 |
| SCK | D5 (默认 SPI) |
| SDA/MOSI | D7 (默认 SPI) |
| VCC | 3.3V |
| GND | GND |

## 依赖库

在 Arduino IDE 中安装以下库：

- **ESP8266WiFi** (ESP8266 核心自带)
- **ESP8266WebServer** (ESP8266 核心自带)
- **WebSocketsServer** - `arduinoWebSockets` by Markus Sattler
- **Adafruit GFX Library**
- **Adafruit ST7735 and ST7789 Library**

## 使用方法

1. 修改代码中的 WiFi 配置：
   ```cpp
   const char* ssid     = "你的WiFi名称";
   const char* password = "你的WiFi密码";
   ```

2. 编译上传到 ESP8266

3. 打开串口监视器(115200)，查看 ESP8266 获取到的 IP 地址

4. 在同一 WiFi 下，用浏览器打开该 IP 地址

5. 选择视频文件，点击 Play 开始播放

### 视频格式要求

- **编码为 H.264 (AVC)**，浏览器可能不支持 H.265/HEVC
- 如果视频是 H.265 编码，用 ffmpeg 转码：
  ```bash
  ffmpeg -i input.mp4 -c:v libx264 -crf 23 -preset fast -c:a aac -b:a 128k output_h264.mp4
  ```

## 技术细节

### 工作原理

```
浏览器                          ESP8266
 <video> 解码视频
    |
 Canvas 抓帧 (128x128)
    |
 二值化 (灰度>128 → 白/黑)
    |
 差分编码 (只提取变化的行)
    |
 WebSocket 发送 ──────────→  接收帧数据
                               |
                            SPI 写入 TFT 屏幕
                               |
                            发送 "OK" ←──────── ACK
```

### 帧协议

每帧通过 WebSocket 二进制消息发送，格式：

- **全帧**: `[0xFF] [0xFF] [2048字节数据]` — 128x128 像素，1-bit/像素
- **局部帧**: `[yStart] [yEnd] [脏行数据]` — 只包含变化区域的行

### 性能优化

| 优化项 | 说明 |
|--------|------|
| 差分帧编码 | JS 端逐行对比前后帧，只发送变化的行范围 |
| 局部刷新 | ESP 端用 `setAddrWindow` 只刷新脏区域，减少 SPI 写入量 |
| SPI 批量写入 | 用 `SPI.writeBytes()` 逐行批量传输，替代逐像素 `write16` |
| 40MHz SPI | SPI 频率从默认提升到 40MHz |
| 双缓冲流水线 | 允许 2 帧同时在传输中，隐藏网络延迟 |
| 智能跳帧 | 大变化帧(>75%行变化)且管线繁忙时自动跳帧，避免堆积卡顿 |
| 缓冲区检测 | 检测 WebSocket 发送缓冲区，堆积时主动跳帧 |
| 强制全帧同步 | 连续跳帧超过 10 次后发全帧，防止画面残留 |

### 内存占用

| 缓冲区 | 大小 |
|--------|------|
| curFrame | 2048 字节 (128x128 / 8) |
| lineBuf (SPI行缓冲) | 256 字节 (128 像素 x 2 字节 RGB565) |

## 注意事项

- 如果 40MHz SPI 不稳定（花屏），可降低为 `SPI.setFrequency(32000000)`
- 显示为 1-bit 黑白画面，适合高对比度视频（如 Bad Apple），非黑白视频会自动转为黑白
- 帧率取决于画面变化量：静态场景可达较高帧率，全屏变化场景帧率会下降
- 彩色视频帧率极低，刷新极慢；黑百与彩色之间切换时，必须将视频**stop**
