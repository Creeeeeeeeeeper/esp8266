#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <WebSocketsServer.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <SPI.h>

// ============ WiFi 配置 ============
const char* ssid     = "";
const char* password = "";

// ============ TFT 引脚定义 ============
#define TFT_CS   D1
#define TFT_DC   D3
#define TFT_RST  D2
#define TFT_LED  D4

Adafruit_ST7735 tft = Adafruit_ST7735(TFT_CS, TFT_DC, TFT_RST);
ESP8266WebServer server(80);
WebSocketsServer ws(81);
uint8_t rotation = 0;

// 当前帧缓冲 - 黑白模式
uint8_t curFrame[2048];
bool hasFrame = false;

// 彩色帧缓冲 - 批量行传输，每批4行
// 协议: [yStart(1B)] [0xFD] [4行 * 256字节 = 1024字节 RGB565]
#define COLOR_BATCH_ROWS 4

// 显示模式: 0=黑白, 1=彩色
uint8_t displayMode = 0;

// SPI行缓冲区：一行128像素 * 2字节 = 256字节
uint8_t lineBuf[256];

// 帧计数与FPS
uint32_t frameCount = 0;
uint32_t fpsCounter = 0;
uint32_t lastFpsTime = 0;
uint8_t currentFps = 0;
bool showFps = false;

// ============ 将1-bit行展开到lineBuf(RGB565) ============
inline void expandLine(uint8_t* src) {
  // src: 16 bytes = 128 bits -> lineBuf: 256 bytes = 128 pixels * 2 bytes
  uint8_t* dst = lineBuf;
  for (int i = 0; i < 16; i++) {
    uint8_t b = src[i];
    for (int bit = 7; bit >= 0; bit--) {
      if (b & (1 << bit)) {
        *dst++ = 0xFF; *dst++ = 0xFF;
      } else {
        *dst++ = 0x00; *dst++ = 0x00;
      }
    }
  }
}

// ============ 全屏绘制 1-bit 帧（批量SPI） ============
void drawFullFrame(uint8_t* data, size_t len) {
  if (len < 2048) return;

  tft.setAddrWindow(0, 0, 127, 127);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);

  for (int row = 0; row < 128; row++) {
    expandLine(data + row * 16);
    SPI.writeBytes(lineBuf, 256);
  }

  digitalWrite(TFT_CS, HIGH);
}

// ============ 局部刷新：只绘制脏行范围（批量SPI） ============
void drawDirtyRegion(uint8_t* data, uint8_t yStart, uint8_t yEnd) {
  tft.setAddrWindow(0, yStart, 127, yEnd);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);

  for (int row = yStart; row <= yEnd; row++) {
    expandLine(data + row * 16);
    SPI.writeBytes(lineBuf, 256);
  }

  digitalWrite(TFT_CS, HIGH);
}

// ============ 彩色批量行写入（RGB565） ============
void drawColorBatch(uint8_t* data, uint8_t yStart, uint8_t rows) {
  uint8_t yEnd = yStart + rows - 1;
  if (yEnd > 127) yEnd = 127;
  tft.setAddrWindow(0, yStart, 127, yEnd);
  digitalWrite(TFT_DC, HIGH);
  digitalWrite(TFT_CS, LOW);
  for (uint8_t i = 0; i < rows && (yStart + i) < 128; i++) {
    // 先拷贝到对齐的lineBuf，避免SPI未对齐访问导致Exception(9)
    memcpy(lineBuf, data + i * 256, 256);
    SPI.writeBytes(lineBuf, 256);
  }
  digitalWrite(TFT_CS, HIGH);
  yield();
}

// ============ 在右下角叠加FPS ============
void drawFpsOverlay() {
  if (!showFps) return;
  char buf[8];
  sprintf(buf, "%d", currentFps);
  int16_t x = 128 - strlen(buf) * 6 - 2;
  int16_t y = 128 - 8 - 1;
  tft.fillRect(x - 1, y - 1, strlen(buf) * 6 + 2, 10, ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(1);
  tft.setCursor(x, y);
  tft.print(buf);
}

// ============ WebSocket 事件 ============
void wsEvent(uint8_t num, WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {
    case WStype_CONNECTED:
      Serial.printf("Client [%u] connected\n", num);
      break;

    case WStype_DISCONNECTED:
      Serial.printf("Client [%u] disconnected\n", num);
      break;

    case WStype_TEXT:
      if (length >= 3 && payload[0] == 'C' && payload[1] == 'L' && payload[2] == 'R') {
        tft.fillScreen(ST7735_BLACK);
        hasFrame = false;
        frameCount = 0;
        Serial.println("Screen cleared");
      }
      else if (length >= 3 && payload[0] == 'R' && payload[1] == 'O' && payload[2] == 'T') {
        rotation = (rotation + 1) % 4;
        tft.setRotation(rotation);
        if (hasFrame) {
          drawFullFrame(curFrame, 2048);
        }
        Serial.printf("Rotation: %d\n", rotation);
      }
      else if (length >= 3 && payload[0] == 'F' && payload[1] == 'P' && payload[2] == 'S') {
        showFps = !showFps;
        if (!showFps && hasFrame) {
          // 关闭FPS时重绘最后一帧以清除叠加
          drawFullFrame(curFrame, 2048);
        }
        Serial.printf("FPS display: %s\n", showFps ? "ON" : "OFF");
      }
      else if (length >= 4 && payload[0] == 'M' && payload[1] == 'O' && payload[2] == 'D' && payload[3] == 'E') {
        displayMode = (payload[4] == '1') ? 1 : 0;
        tft.fillScreen(ST7735_BLACK);
        hasFrame = false;
        Serial.printf("Display mode: %s\n", displayMode ? "Color" : "B/W");
      }
      break;

    case WStype_BIN:
      // 黑白协议: [yStart(1B)] [yEnd(1B)] [data(行数*16 bytes)]
      // 或全帧: [0xFF] [0xFF] [2048 bytes]
      // 彩色协议: [yStart(1B)] [0xFD] [4行*256字节 = 1024字节 RGB565]
      if (length >= 2) {
        uint8_t yStart = payload[0];
        uint8_t yEnd   = payload[1];
        uint8_t* frameData = payload + 2;
        size_t dataLen = length - 2;

        // 彩色模式：yEnd==0xFD 表示批量行数据（4行一批）
        if (displayMode == 1 && yEnd == 0xFD && yStart < 128 && dataLen >= COLOR_BATCH_ROWS * 256) {
          if (ESP.getFreeHeap() < 4096) {
            // 内存过低，跳过这帧，只回ACK
            Serial.println("Low mem, skip color batch");
          } else {
            uint8_t rows = COLOR_BATCH_ROWS;
            if (yStart + rows > 128) rows = 128 - yStart;
            drawColorBatch(frameData, yStart, rows);
            hasFrame = true;
          }
        }
        // 黑白全帧模式
        else if (displayMode == 0 && yStart == 0xFF && yEnd == 0xFF && dataLen >= 2048) {
          memcpy(curFrame, frameData, 2048);
          hasFrame = true;
          drawFullFrame(curFrame, 2048);
        }
        // 黑白局部更新模式
        else if (displayMode == 0 && yStart <= yEnd && yEnd < 128) {
          int expectedLen = (yEnd - yStart + 1) * 16;
          if ((int)dataLen >= expectedLen) {
            memcpy(curFrame + yStart * 16, frameData, expectedLen);
            hasFrame = true;
            drawDirtyRegion(curFrame, yStart, yEnd);
          }
        }

        frameCount++;
        fpsCounter++;
        uint32_t now = millis();
        if (now - lastFpsTime >= 1000) {
          currentFps = fpsCounter;
          fpsCounter = 0;
          lastFpsTime = now;
        }
        drawFpsOverlay();
        ws.sendTXT(num, "OK");
      }
      break;

    default:
      break;
  }
}

// ============ 网页 HTML ============
const char HTML_PAGE[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html><head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>TFT Player</title>
<style>
*{margin:0;padding:0;box-sizing:border-box}
body{font-family:sans-serif;background:#1a1a2e;color:#eee;display:flex;justify-content:center;align-items:center;min-height:100vh}
.c{background:#16213e;border-radius:12px;padding:16px;width:90%;max-width:360px}
h1{text-align:center;font-size:18px;margin-bottom:10px}
.pv{display:flex;justify-content:center;margin-bottom:10px}
canvas{border:2px solid #0f3460;background:#000;image-rendering:pixelated}
input[type=file]{width:100%;padding:6px;background:#1a1a2e;color:#eee;border:1px solid #0f3460;border-radius:6px;font-size:13px;margin-bottom:8px}
.i{display:flex;justify-content:space-between;font-size:11px;color:#888;margin-bottom:4px}
input[type=range]{width:100%;margin-bottom:8px}
.r{display:flex;gap:6px;flex-wrap:wrap;margin-bottom:6px}
.b{flex:1;padding:10px 6px;border:none;border-radius:8px;font-size:13px;font-weight:bold;cursor:pointer;min-width:60px;color:#fff}
.st{text-align:center;color:#53d769;font-size:12px;min-height:18px}
video{display:none}
</style></head><body>
<div class="c">
<h1>TFT Video Player</h1>
<div class="pv"><canvas id="cv" width="128" height="128" style="width:256px;height:256px"></canvas></div>
<input type="file" id="fi" accept="video/*">
<div class="i"><span id="ti">0:00/0:00</span><span id="fp">FPS:--</span></div>
<input type="range" id="sb" min="0" max="1000" value="0">
<div class="r">
<button class="b" style="background:#27ae60" onclick="doPlay()">Play</button>
<button class="b" style="background:#e67e22" onclick="doPause()">Pause</button>
<button class="b" style="background:#e74c3c" onclick="doStop()">Stop</button>
<button class="b" style="background:#7b2d8e" onclick="doRotate()">Rot</button>
<button class="b" id="fb" style="background:#2d6b8e" onclick="doToggleFps()">FPS</button>
</div>
<div class="r">
<button class="b" id="mb" style="background:#c0392b" onclick="doToggleMode()">B/W</button>
</div>
<div class="st" id="st">Select a video file</div>
</div>
<video id="vid" playsinline></video>
<script>
var cv=document.getElementById('cv');
var ctx=cv.getContext('2d',{willReadFrequently:true});
ctx.imageSmoothingEnabled=false;
var vid=document.getElementById('vid');
var sb=document.getElementById('sb');
var stEl=document.getElementById('st');
var sock=null,playing=false,videoReady=false;
var fpsTick=0,fpsLast=Date.now(),fpsVal=0;

// 显示模式: 0=黑白, 1=彩色
var displayMode=0;

// 彩色批量参数
var COLOR_BATCH_ROWS=4;  // 每批4行
var COLOR_BATCHES=32;    // 128/4=32批

// 双缓冲流水线：允许2帧同时在路上
var pendingAcks=0;
var MAX_PENDING=2;

// 上一帧的1-bit数据，用于差分（黑白模式）
var prevBuf=null;
var isFirstFrame=true;

// 彩色模式：当前帧发送状态
var colorSendBatch=0;    // 当前正在发送的批次号
var colorFrameBuf=null;  // 当前帧的完整RGB565数据
var colorSending=false;  // 是否正在分批发送中
var prevColorBuf=null;   // 上一帧彩色数据，用于差分

// 帧率限制
var TARGET_FPS=12;
var FRAME_INTERVAL=1000/TARGET_FPS;  // ~83ms
var lastFrameTime=0;

// 视频绘制参数（保持原比例，长边占满128，短边尽量大，少留黑边）
var drawX=0,drawY=0,drawW=128,drawH=128;

function calcDrawRect(){
  var vw=vid.videoWidth,vh=vid.videoHeight;
  if(!vw||!vh){drawX=0;drawY=0;drawW=128;drawH=128;return;}
  var ratio=vw/vh;
  if(ratio>1){
    // 横屏视频：宽度占满128，高度按比例
    drawW=128;
    drawH=Math.round(128/ratio);
    // 如果黑边太多(短边<90%屏幕)，放大短边到90%
    var minH=Math.round(128*0.9);
    if(drawH<minH){drawH=minH;drawW=Math.round(drawH*ratio);}
    drawX=Math.floor((128-drawW)/2);
    drawY=Math.floor((128-drawH)/2);
  }else if(ratio<1){
    // 竖屏视频：高度占满128，宽度按比例
    drawH=128;
    drawW=Math.round(128*ratio);
    var minW=Math.round(128*0.9);
    if(drawW<minW){drawW=minW;drawH=Math.round(drawW/ratio);}
    drawX=Math.floor((128-drawW)/2);
    drawY=Math.floor((128-drawH)/2);
  }else{
    drawX=0;drawY=0;drawW=128;drawH=128;
  }
}

function wsConnect(){
  sock=new WebSocket('ws://'+location.hostname+':81/');
  sock.binaryType='arraybuffer';
  sock.onopen=function(){stEl.innerText='Connected';};
  sock.onmessage=function(e){
    if(typeof e.data==='string'&&e.data==='OK'){
      if(pendingAcks>0)pendingAcks--;
      fpsTick++;
      var now=Date.now();
      if(now-fpsLast>=1000){
        fpsVal=fpsTick;fpsTick=0;fpsLast=now;
        document.getElementById('fp').innerText='FPS:'+fpsVal;
      }
      if(colorSending&&colorFrameBuf){
        sendNextColorBatch();
      }
    }
  };
  sock.onclose=function(){
    stEl.innerText='Reconnecting...';
    setTimeout(wsConnect,1000);
  };
  sock.onerror=function(){sock.close();};
}
wsConnect();

document.getElementById('fi').addEventListener('change',function(e){
  var file=e.target.files[0];
  if(!file)return;
  vid.src=URL.createObjectURL(file);
  vid.load();
  vid.onloadeddata=function(){
    videoReady=true;
    isFirstFrame=true;
    prevBuf=null;
    sb.max=Math.floor(vid.duration*1000);
    calcDrawRect();
    ctx.fillStyle='#000';
    ctx.fillRect(0,0,128,128);
    ctx.drawImage(vid,drawX,drawY,drawW,drawH);
    stEl.innerText='Ready: '+file.name;
    updateTime();
  };
});

function updateTime(){
  var cur=vid.currentTime||0;
  var dur=vid.duration||0;
  function fmt(t){
    var m=Math.floor(t/60);
    var s=Math.floor(t%60);
    return m+':'+(s<10?'0':'')+s;
  }
  document.getElementById('ti').innerText=fmt(cur)+'/'+fmt(dur);
  sb.value=Math.floor(cur*1000);
}

sb.addEventListener('input',function(){
  if(!videoReady)return;
  vid.currentTime=this.value/1000;
  isFirstFrame=true;
  prevBuf=null;
  updateTime();
});

function captureFrame(){
  // 保持原比例绘制到canvas
  ctx.fillStyle='#000';
  ctx.fillRect(0,0,128,128);
  ctx.drawImage(vid,drawX,drawY,drawW,drawH);
  var imgData=ctx.getImageData(0,0,128,128);
  var px=imgData.data;

  if(displayMode===0){
    // 黑白模式：二值化
    var buf=new Uint8Array(2048);
    for(var row=0;row<128;row++){
      for(var col=0;col<128;col++){
        var i=(row*128+col)*4;
        var gray=px[i]*0.299+px[i+1]*0.587+px[i+2]*0.114;
        if(gray>128){
          buf[row*16+(col>>3)]|=(1<<(7-(col&7)));
        }
      }
    }
    return buf;
  }else{
    // 彩色模式：转RGB565，逐行返回
    // 返回格式: {row: 行号, data: Uint8Array(256)}
    var colorData=new Uint8Array(256*128);
    for(var row=0;row<128;row++){
      for(var col=0;col<128;col++){
        var i=(row*128+col)*4;
        var r=px[i],g=px[i+1],b=px[i+2];
        // RGB565: RRRR RGGG GGGB BBBB
        var rgb565=(((r&0xF8)<<8)|((g&0xFC)<<3)|((b&0xF8)>>3));
        colorData[row*256+col*2]=(rgb565>>8)&0xFF;
        colorData[row*256+col*2+1]=rgb565&0xFF;
      }
    }
    return colorData;
  }
}

// 跳帧计数器，连续跳帧后强制发全帧修正
var skipCount=0;


// 发送下一批彩色数据
function sendNextColorBatch(){
  if(!colorSending||!colorFrameBuf||!sock||sock.readyState!==1)return;
  if(colorSendBatch>=COLOR_BATCHES){
    // 全部批次发完，保存当前帧用于下次差分
    colorSending=false;
    prevColorBuf=colorFrameBuf;
    colorFrameBuf=null;
    return;
  }
  // 差分：检查这一批的数据是否和上一帧一样
  var batchStart=colorSendBatch*COLOR_BATCH_ROWS*256;
  var batchLen=COLOR_BATCH_ROWS*256;
  var dirty=false;
  if(!prevColorBuf){
    dirty=true;
  }else{
    for(var i=0;i<batchLen;i++){
      if(colorFrameBuf[batchStart+i]!==prevColorBuf[batchStart+i]){dirty=true;break;}
    }
  }
  if(!dirty){
    // 这批没变化，跳到下一批
    colorSendBatch++;
    sendNextColorBatch();
    return;
  }
  // 发送: [yStart][0xFD][8行*256字节]
  var yStart=colorSendBatch*COLOR_BATCH_ROWS;
  var pkt=new Uint8Array(2+batchLen);
  pkt[0]=yStart;
  pkt[1]=0xFD;
  pkt.set(colorFrameBuf.subarray(batchStart,batchStart+batchLen),2);
  sock.send(pkt.buffer);
  pendingAcks++;
  colorSendBatch++;
  // 不再立即发下一批，等ACK回来再发
}

function sendFrame(){
  if(!sock||sock.readyState!==1)return false;

  // 彩色模式下如果还在分批发送中，不抓新帧
  if(colorSending)return false;

  // 黑白模式检查pendingAcks
  if(displayMode===0&&pendingAcks>=MAX_PENDING)return false;

  // 发送缓冲区堆积时跳过
  if(sock.bufferedAmount>1024){
    skipCount++;
    return false;
  }

  var buf=captureFrame();

  // 彩色模式：启动分批发送流程
  if(displayMode===1){
    colorFrameBuf=buf;
    colorSendBatch=0;
    colorSending=true;
    sendNextColorBatch();
    return true;
  }

  // 黑白模式：差分发送（原有逻辑不变）
  if(isFirstFrame||!prevBuf||skipCount>=10){
    var pkt=new Uint8Array(2+2048);
    pkt[0]=0xFF;
    pkt[1]=0xFF;
    pkt.set(buf,2);
    sock.send(pkt.buffer);
    prevBuf=buf;
    isFirstFrame=false;
    skipCount=0;
    pendingAcks++;
    return true;
  }

  var yStart=-1,yEnd=-1;
  for(var row=0;row<128;row++){
    var off=row*16;
    var dirty=false;
    for(var b=0;b<16;b++){
      if(buf[off+b]!==prevBuf[off+b]){dirty=true;break;}
    }
    if(dirty){
      if(yStart===-1)yStart=row;
      yEnd=row;
    }
  }

  if(yStart===-1){
    fpsTick++;
    skipCount=0;
    return true;
  }

  var dirtyRows=yEnd-yStart+1;
  if(dirtyRows>96&&pendingAcks>0){
    skipCount++;
    return true;
  }

  var dataLen=dirtyRows*16;
  var pkt=new Uint8Array(2+dataLen);
  pkt[0]=yStart;
  pkt[1]=yEnd;
  pkt.set(buf.subarray(yStart*16,(yEnd+1)*16),2);
  sock.send(pkt.buffer);
  prevBuf=buf;
  pendingAcks++;
  skipCount=0;
  return true;
}

function playLoop(){
  if(!playing)return;
  if(vid.ended||vid.paused){
    playing=false;
    stEl.innerText='Finished';
    return;
  }
  updateTime();
  var now=Date.now();
  if(now-lastFrameTime>=FRAME_INTERVAL){
    if(!colorSending&&pendingAcks<MAX_PENDING){
      sendFrame();
      lastFrameTime=now;
    }
  }
  requestAnimationFrame(playLoop);
}

function doPlay(){
  if(!videoReady){stEl.innerText='Load a video first!';return;}
  if(playing)return;
  playing=true;
  pendingAcks=0;
  var p=vid.play();
  if(p&&p.then){
    p.then(function(){
      stEl.innerText='Playing...';
      playLoop();
    }).catch(function(err){
      playing=false;
      stEl.innerText='Play error: '+err.message;
    });
  }else{
    stEl.innerText='Playing...';
    playLoop();
  }
}

function doPause(){
  playing=false;
  vid.pause();
  stEl.innerText='Paused';
}

function doStop(){
  playing=false;
  vid.pause();
  vid.currentTime=0;
  pendingAcks=0;
  isFirstFrame=true;
  prevBuf=null;
  prevColorBuf=null;
  colorSending=false;
  colorFrameBuf=null;
  updateTime();
  if(sock&&sock.readyState===1)sock.send('CLR');
  ctx.fillStyle='#000';
  ctx.fillRect(0,0,128,128);
  stEl.innerText='Stopped';
}

function doRotate(){
  if(sock&&sock.readyState===1)sock.send('ROT');
}

var fpsOn=false;
function doToggleFps(){
  if(sock&&sock.readyState===1)sock.send('FPS');
  fpsOn=!fpsOn;
  var btn=document.getElementById('fb');
  btn.style.background=fpsOn?'#27ae60':'#2d6b8e';
  btn.innerText=fpsOn?'FPS ON':'FPS';
}

function doToggleMode(){
  if(playing){stEl.innerText='Stop first';return;}
  displayMode=(displayMode===0)?1:0;
  prevColorBuf=null;
  colorSending=false;
  colorFrameBuf=null;
  prevBuf=null;
  isFirstFrame=true;
  if(sock&&sock.readyState===1){
    sock.send('MODE'+(displayMode?'1':'0'));
  }
  var btn=document.getElementById('mb');
  btn.style.background=displayMode?'#27ae60':'#c0392b';
  btn.innerText=displayMode?'Color':'B/W';
  stEl.innerText=displayMode?'Color mode':'B/W mode';
}
</script>
</body>
</html>
)rawliteral";

// ============ HTTP: 主页 ============
void handleRoot() {
  server.send_P(200, "text/html", HTML_PAGE);
}

// ============ 启动画面 ============
void showBootScreen(const char* line1, const char* line2) {
  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_CYAN);
  tft.setTextSize(1);
  tft.setCursor(2, 10);
  tft.print(line1);
  tft.setTextColor(ST7735_WHITE);
  tft.setCursor(2, 30);
  tft.print(line2);
}

// ============ Setup ============
void setup() {
  Serial.begin(115200);
  Serial.println();

  pinMode(TFT_LED, OUTPUT);
  digitalWrite(TFT_LED, HIGH);

  tft.initR(INITR_144GREENTAB);
  tft.setRotation(0);
  tft.fillScreen(ST7735_BLACK);

  SPI.setFrequency(40000000);

  memset(curFrame, 0, 2048);

  showBootScreen("Connecting WiFi...", ssid);

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");

  int dots = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
    tft.setCursor(2 + dots * 6, 50);
    tft.setTextColor(ST7735_YELLOW);
    tft.print(".");
    dots++;
    if (dots > 20) {
      dots = 0;
      tft.fillRect(0, 50, 128, 10, ST7735_BLACK);
    }
  }

  Serial.println();
  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  tft.fillScreen(ST7735_BLACK);
  tft.setTextColor(ST7735_GREEN);
  tft.setTextSize(1);
  tft.setCursor(2, 10);
  tft.print("WiFi Connected!");
  tft.setTextColor(ST7735_YELLOW);
  tft.setCursor(2, 30);
  tft.print("IP:");
  tft.setTextSize(2);
  tft.setCursor(2, 42);
  tft.print(WiFi.localIP());
  tft.setTextColor(ST7735_WHITE);
  tft.setTextSize(1);
  tft.setCursor(2, 70);
  tft.print("Open IP in browser");
  tft.setCursor(2, 82);
  tft.print("to play video");
  tft.setTextColor(ST7735_CYAN);
  tft.setCursor(2, 100);
  tft.print("Video Player Ready");

  server.on("/", HTTP_GET, handleRoot);
  server.begin();
  Serial.println("HTTP server on port 80");

  ws.begin();
  ws.onEvent(wsEvent);
  Serial.println("WebSocket server on port 81");
  Serial.printf("Free heap: %u bytes\n", ESP.getFreeHeap());
}

// ============ Loop ============
void loop() {
  ws.loop();
  server.handleClient();
  yield();
}
