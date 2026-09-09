/*
 * IoT DC Power Meter
 * ESP32 + INA219 + OLED SSD1306 + Web Dashboard
 *
 * โหลดทดสอบ: มอเตอร์เกียร์ ZGA37RG 12V 300rpm (หมุนเปล่า ~0.12A)
 *
 * การต่อสาย
 *   INA219  VCC -> 3.3V   GND -> GND   SDA -> GPIO21   SCL -> GPIO22
 *           Vin+ -> ขั้วบวกแหล่งจ่าย    Vin- -> ขั้วบวกมอเตอร์
 *   OLED    VCC -> 3.3V   GND -> GND   SDA -> GPIO21   SCL -> GPIO22
 *   กราวด์ ESP32 ต้องต่อร่วมกับขั้วลบแหล่งจ่ายเสมอ
 *
 * ปุ่ม BOOT (GPIO0)
 *   กดสั้น  = สลับย่านการวัด (ละเอียด <-> กว้าง)
 *   กดค้าง  = รีเซ็ตค่าพลังงานสะสมและค่าสูงสุด
 *
 * ไลบรารีที่ต้องติดตั้ง
 *   Adafruit INA219, Adafruit SSD1306, Adafruit GFX
 */

#include <Wire.h>
#include <Adafruit_INA219.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <WiFi.h>
#include <WebServer.h>

// ---------------- ตั้งค่า ----------------

#define SIM_MODE       false        // true = ใช้ค่าจำลอง ไม่ต้องต่อฮาร์ดแวร์
#define SDA_PIN        21
#define SCL_PIN        22
#define BTN_PIN        0            // ปุ่ม BOOT บนบอร์ด
#define OLED_ADDR      0x3C
#define OLED_W         128
#define OLED_H         64

#define SAMPLES        20           // จำนวนครั้งที่อ่านแล้วเฉลี่ย
#define SAMPLE_MS      200          // คาบการอ่านค่า
#define OLED_MS        250          // คาบการอัปเดตจอ
#define LONG_PRESS_MS  1200

const char* WIFI_SSID = "ใส่ชื่อไวไฟ";
const char* WIFI_PASS = "ใส่รหัสผ่าน";
const char* AP_SSID   = "PowerMeter";   // ใช้เมื่อต่อไวไฟบ้านไม่ได้
const char* AP_PASS   = "12345678";

// ---------------- ตัวแปรร่วม ----------------

Adafruit_INA219 ina219;
Adafruit_SSD1306 display(OLED_W, OLED_H, &Wire, -1);
WebServer server(80);

float voltage_V  = 0;      // แรงดันที่โหลดได้รับ
float current_mA = 0;
float power_W    = 0;
double energy_Wh = 0;
float peak_mA    = 0;      // จับกระแสสูงสุด เช่น ตอนมอเตอร์สตาร์ท
float peak_W     = 0;

uint8_t range = 0;         // 0 = ละเอียด 16V/400mA, 1 = กว้าง 32V/2A
bool sensorOK  = false;
bool oledOK    = false;
bool apMode    = false;
String ipText  = "-";

unsigned long lastSample = 0;
unsigned long lastOled   = 0;
unsigned long lastEnergy = 0;
unsigned long btnDownAt  = 0;
bool btnHandled = false;

// ---------------- INA219 ----------------

void applyRange() {
  if (range == 0) ina219.setCalibration_16V_400mA();
  else            ina219.setCalibration_32V_2A();
}

const char* rangeName() {
  return range == 0 ? "16V/400mA" : "32V/2A";
}

// สแกนหาอุปกรณ์บนบัส I2C แล้วพิมพ์ผลออก Serial
void scanI2C() {
  Serial.println(F("\n-- I2C scan --"));
  uint8_t found = 0;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  พบอุปกรณ์ที่ 0x%02X", addr);
      if (addr == 0x40) Serial.print(F("  <- INA219"));
      if (addr == 0x3C) Serial.print(F("  <- OLED"));
      Serial.println();
      found++;
    }
  }
  if (found == 0) Serial.println(F("  ไม่พบอะไรเลย -> เช็คสาย SDA/SCL และกราวด์ร่วม"));
  Serial.println(F("--------------"));
}

// ---------------- อ่านค่า ----------------

void readSensor() {
  if (SIM_MODE) {
    // จำลองมอเตอร์: กระแสฐาน 120mA แกว่งเล็กน้อย
    float t = millis() / 1000.0;
    voltage_V  = 12.0 + 0.05 * sin(t * 0.7);
    current_mA = 120.0 + 25.0 * sin(t * 0.3) + random(-30, 30) / 10.0;
    if (current_mA < 0) current_mA = 0;
  } else if (sensorOK) {
    float sumV = 0, sumI = 0;
    for (uint8_t i = 0; i < SAMPLES; i++) {
      sumV += ina219.getBusVoltage_V();
      sumI += ina219.getCurrent_mA();
      delayMicroseconds(200);
    }
    voltage_V  = sumV / SAMPLES;
    current_mA = sumI / SAMPLES;
    if (current_mA < 0) current_mA = 0;   // ตัดค่าติดลบจากสัญญาณรบกวน
  } else {
    return;
  }

  power_W = voltage_V * current_mA / 1000.0;

  // สะสมพลังงาน: W x ชั่วโมง
  unsigned long now = millis();
  if (lastEnergy > 0) {
    energy_Wh += power_W * ((now - lastEnergy) / 3600000.0);
  }
  lastEnergy = now;

  if (current_mA > peak_mA) peak_mA = current_mA;
  if (power_W  > peak_W)  peak_W  = power_W;
}

void resetTotals() {
  energy_Wh = 0;
  peak_mA   = 0;
  peak_W    = 0;
  lastEnergy = millis();
}

// ---------------- จอ OLED ----------------

void drawOled() {
  if (!oledOK) return;
  display.clearDisplay();

  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.print(rangeName());
  display.setCursor(OLED_W - 24, 0);
  display.print(apMode ? "AP" : (WiFi.status() == WL_CONNECTED ? "WiFi" : "--"));

  display.setTextSize(2);
  display.setCursor(0, 14);
  display.print(voltage_V, 2);
  display.setTextSize(1);
  display.print(" V");

  display.setTextSize(2);
  display.setCursor(0, 34);
  display.print(current_mA, 1);
  display.setTextSize(1);
  display.print(" mA");

  display.setCursor(0, 54);
  display.print(power_W, 3);
  display.print("W  ");
  display.print(energy_Wh, 4);
  display.print("Wh");

  display.display();
}

// ---------------- หน้าเว็บ ----------------

const char PAGE_HTML[] PROGMEM = R"HTMLPAGE(<!DOCTYPE html>
<html lang="th"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DC Power Meter</title>
<style>
:root{--bg:#0c1220;--bg2:#141d33;--edge:#22304d;--tx:#e8eef8;--dim:#7d8daa;
--cy:#4fd8c6;--am:#f5b642;--vi:#9b8cf0;--red:#ef6a5a}
*{margin:0;box-sizing:border-box}
body{min-height:100vh;background:radial-gradient(120% 80% at 50% 0%,#182444 0%,var(--bg) 62%);
color:var(--tx);font:15px/1.6 system-ui,-apple-system,"Segoe UI",sans-serif;
padding:26px 20px 46px;max-width:680px;margin:0 auto;-webkit-font-smoothing:antialiased}
header{display:flex;justify-content:space-between;align-items:center}
h1{font-size:15px;font-weight:600;letter-spacing:.04em}
#st{font-size:12px;color:var(--dim);border:1px solid var(--edge);border-radius:999px;
padding:4px 11px;background:rgba(255,255,255,.02)}
#st.bad{color:var(--red);border-color:#4a2a2a}
.hero{margin:26px 0 4px;display:flex;align-items:baseline;gap:10px}
.hero b{font:300 62px/1 ui-monospace,Menlo,Consolas,monospace;font-variant-numeric:tabular-nums;
color:var(--cy);text-shadow:0 0 26px rgba(79,216,198,.42)}
.hero i{font-style:normal;font-size:19px;color:var(--dim)}
.hero small{margin-left:auto;font-size:12px;color:var(--dim);text-align:right;line-height:1.4}
canvas{width:100%;height:186px;display:block;margin:4px 0 22px}
.row{display:flex;border-top:1px solid var(--edge);border-bottom:1px solid var(--edge)}
.q{flex:1;padding:15px 4px 14px;position:relative}
.q+.q{border-left:1px solid var(--edge)}
.q b{display:block;font:400 25px/1.15 ui-monospace,Menlo,Consolas,monospace;
font-variant-numeric:tabular-nums}
.q span{font-size:11.5px;color:var(--dim)}
#nv b{color:var(--vi)}
#ni b{color:var(--am)}
#ne b{color:var(--tx)}
.btns{display:flex;gap:11px;margin-top:20px}
button{flex:1;font:inherit;font-size:14px;color:var(--tx);cursor:pointer;
background:linear-gradient(180deg,rgba(255,255,255,.05),rgba(255,255,255,.01));
border:1px solid var(--edge);border-radius:10px;padding:12px}
button:active{background:rgba(79,216,198,.14);border-color:var(--cy)}
</style></head><body>
<header><h1>DC Power Meter</h1><div id="st">กำลังอ่านค่า</div></header>

<div class="hero">
<b id="p">–</b><i>W</i>
<small><span id="pk">–</span><br><span id="up">–</span></small>
</div>
<canvas id="c"></canvas>

<div class="row">
<div class="q" id="nv"><b id="v">–</b><span>แรงดัน · โวลต์</span></div>
<div class="q" id="ni"><b id="i">–</b><span>กระแส · มิลลิแอมป์</span></div>
<div class="q" id="ne"><b id="e">–</b><span>พลังงานสะสม · Wh</span></div>
</div>

<div class="btns">
<button onclick="cmd('/mode')">สลับย่านการวัด</button>
<button onclick="cmd('/reset')">เริ่มนับใหม่</button>
</div>

<script>
var H=[],MAX=190,c=document.getElementById('c'),g=c.getContext('2d');
function fit(){c.width=c.clientWidth*devicePixelRatio;c.height=c.clientHeight*devicePixelRatio;
g.setTransform(devicePixelRatio,0,0,devicePixelRatio,0,0);draw()}
onresize=fit;
function draw(){
 var w=c.clientWidth,h=c.clientHeight,k,mx=0.01;
 g.clearRect(0,0,w,h);
 for(k=0;k<H.length;k++)if(H[k]>mx)mx=H[k];
 mx*=1.35;
 g.strokeStyle='#1b2740';g.lineWidth=1;
 for(k=1;k<4;k++){var y=h*k/4;g.beginPath();g.moveTo(0,y);g.lineTo(w,y);g.stroke()}
 g.fillStyle='#7d8daa';g.font='11px ui-monospace,Menlo,monospace';
 g.fillText(mx.toFixed(2)+' W',0,12);
 if(H.length<2)return;
 var X=function(n){return w*n/(MAX-1)},Y=function(v){return h-8-(v/mx)*(h-30)};
 var grd=g.createLinearGradient(0,0,0,h);
 grd.addColorStop(0,'rgba(79,216,198,.30)');grd.addColorStop(1,'rgba(79,216,198,0)');
 g.beginPath();g.moveTo(X(0),h);
 for(k=0;k<H.length;k++)g.lineTo(X(k),Y(H[k]));
 g.lineTo(X(H.length-1),h);g.closePath();g.fillStyle=grd;g.fill();
 g.shadowColor='rgba(79,216,198,.75)';g.shadowBlur=12;
 g.strokeStyle='#4fd8c6';g.lineWidth=2;g.lineJoin='round';
 g.beginPath();
 for(k=0;k<H.length;k++){k?g.lineTo(X(k),Y(H[k])):g.moveTo(X(k),Y(H[k]))}
 g.stroke();
 g.fillStyle='#8ff0e2';g.beginPath();
 g.arc(X(H.length-1),Y(H[H.length-1]),4,0,7);g.fill();
 g.shadowBlur=0;
}
function set(id,t){document.getElementById(id).textContent=t}
function tick(){
 fetch('/data').then(function(r){return r.json()}).then(function(d){
  set('p',d.p.toFixed(3));set('v',d.v.toFixed(2));
  set('i',d.i.toFixed(1));set('e',d.e.toFixed(4));
  set('pk','สูงสุด '+d.pi.toFixed(1)+' mA');
  set('up','เปิดมาแล้ว '+Math.floor(d.up/60)+' นาที');
  var s='ย่าน '+d.r;
  if(d.sim)s='โหมดค่าจำลอง';
  if(!d.ok)s='เซนเซอร์ไม่ตอบสนอง';
  set('st',s);document.getElementById('st').className=d.ok?'':'bad';
  H.push(d.p);if(H.length>MAX)H.shift();
  draw();
 }).catch(function(){
  set('st','ขาดการเชื่อมต่อ');document.getElementById('st').className='bad';
 });
}
function cmd(u){fetch(u).then(tick)}
fit();tick();setInterval(tick,500);
</script></body></html>)HTMLPAGE";

void handleRoot() {
  server.send_P(200, "text/html; charset=utf-8", PAGE_HTML);
}

void handleData() {
  char buf[240];
  snprintf(buf, sizeof(buf),
    "{\"v\":%.3f,\"i\":%.2f,\"p\":%.4f,\"e\":%.5f,"
    "\"pi\":%.2f,\"pp\":%.4f,\"r\":\"%s\",\"ok\":%s,\"sim\":%s,\"up\":%lu}",
    voltage_V, current_mA, power_W, energy_Wh,
    peak_mA, peak_W, rangeName(),
    (sensorOK || SIM_MODE) ? "true" : "false",
    SIM_MODE ? "true" : "false",
    millis() / 1000);
  server.send(200, "application/json", buf);
}

void handleReset() {
  resetTotals();
  server.send(200, "text/plain", "ok");
}

void handleMode() {
  range = 1 - range;
  applyRange();
  server.send(200, "text/plain", rangeName());
}

// ---------------- ไวไฟ ----------------

void startWiFi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print(F("กำลังต่อไวไฟ"));
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) {
    delay(400);
    Serial.print('.');
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    ipText = WiFi.localIP().toString();
    Serial.println("ต่อไวไฟสำเร็จ  เปิด http://" + ipText);
  } else {
    // ต่อไม่ได้ก็ปล่อยไวไฟของตัวเอง จะได้สาธิตที่ไหนก็ได้
    apMode = true;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(AP_SSID, AP_PASS);
    ipText = WiFi.softAPIP().toString();
    Serial.printf("เปิดโหมด AP  ชื่อ %s  รหัส %s\n", AP_SSID, AP_PASS);
    Serial.println("เปิด http://" + ipText);
  }
}

// ---------------- ปุ่ม ----------------

void handleButton() {
  bool down = digitalRead(BTN_PIN) == LOW;

  if (down && btnDownAt == 0) {
    btnDownAt = millis();
    btnHandled = false;
  }

  if (down && !btnHandled && millis() - btnDownAt > LONG_PRESS_MS) {
    resetTotals();
    btnHandled = true;
    Serial.println(F("รีเซ็ตค่าสะสมแล้ว"));
  }

  if (!down && btnDownAt > 0) {
    if (!btnHandled) {
      range = 1 - range;
      applyRange();
      Serial.printf("เปลี่ยนย่านเป็น %s\n", rangeName());
    }
    btnDownAt = 0;
  }
}

// ---------------- setup / loop ----------------

void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println(F("\nIoT DC Power Meter"));

  pinMode(BTN_PIN, INPUT_PULLUP);
  Wire.begin(SDA_PIN, SCL_PIN);
  scanI2C();

  oledOK = display.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR);
  if (oledOK) {
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println(F("Power Meter"));
    display.println(F("starting..."));
    display.display();
  } else {
    Serial.println(F("ไม่พบจอ OLED"));
  }

  if (!SIM_MODE) {
    sensorOK = ina219.begin();
    if (sensorOK) {
      applyRange();
      Serial.printf("INA219 พร้อม  ย่าน %s\n", rangeName());
    } else {
      Serial.println(F("ไม่พบ INA219 -> เช็คสายและกราวด์ร่วม"));
    }
  } else {
    Serial.println(F("ทำงานในโหมดค่าจำลอง"));
  }

  startWiFi();

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/reset", handleReset);
  server.on("/mode", handleMode);
  server.begin();

  lastEnergy = millis();

  if (oledOK) {
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println(apMode ? F("AP: PowerMeter") : F("WiFi ok"));
    display.println(ipText);
    display.display();
    delay(2500);
  }
}

void loop() {
  server.handleClient();
  handleButton();

  unsigned long now = millis();

  if (now - lastSample >= SAMPLE_MS) {
    lastSample = now;
    readSensor();
  }

  if (now - lastOled >= OLED_MS) {
    lastOled = now;
    drawOled();
  }
}
