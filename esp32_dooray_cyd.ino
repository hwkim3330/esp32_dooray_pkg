// esp32_dooray_cyd.ino
// Board: ESP32‑2432S028R (a.k.a. XH32S, “Cheap Yellow Display”)
// Display: 2.8″ ST7789 320×240 (SPI Mode 3) + XPT2046 resistive touch
// Purpose : Show today’s reservations for 4 rooms + simple “예약” tab
/*********************************************************************
* PIN MAP (ST7789 variant – dual USB port board)
* ┌──────── LCD ────────┬────────── Touch (XPT2046) ───────────────┐
* │MOSI : GPIO 13       │  DIN  : GPIO 32                          │
* │SCLK : GPIO 14       │  DO   : GPIO 39                          │
* │CS   : GPIO 15       │  CLK  : GPIO 25                          │
* │DC   : GPIO 2        │  CS   : GPIO 33                          │
* │RST  : –1 (tied)     │  IRQ  : GPIO 36                          │
* │BL   : GPIO 27       │                                          │
* └─────────────────────┴──────────────────────────────────────────┘
*********************************************************************/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <time.h>

/* -------- Wi‑Fi -------- */
const char* WIFI_SSID = "Smart Global";
const char* WIFI_PASS = "";           // open

/* -------- Dooray -------- */
const char* TOKEN = "s701wolho5si:IvK8-thPTzGbr8DiA2bZ8Q";
const char* RESOURCE_IDS = "3868297860122914233,3868312518617103681,3868312634809468080,3868312748489680534";

/* -------- LCD / Touch -------- */
TFT_eSPI tft = TFT_eSPI();            // pins via User_Setup.h
#define TFT_BL 27
#define TOUCH_CS 33
#define TOUCH_IRQ 36
XPT2046_Touchscreen ts(TOUCH_CS, TOUCH_IRQ);
bool isTouched() { return ts.touched(); }
#define CALIB_X_MIN  300
#define CALIB_X_MAX  3800
#define CALIB_Y_MIN  300
#define CALIB_Y_MAX  3800

/* -------- Globals -------- */
enum Page { PAGE_TODAY, PAGE_RESERVE };
Page currentPage = PAGE_TODAY;
uint32_t lastRefresh = 0;

/* -------- Utils -------- */
String isoDate(const char* timeHHMMSS){
  struct tm ti; if(!getLocalTime(&ti)) return "";
  char d[32]; strftime(d,sizeof(d),"%Y-%m-%d", &ti);
  return String(d) + "T" + String(timeHHMMSS) + "+09:00";
}

bool httpsGET(const String& url, String& body){
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.begin(client,url);
  http.addHeader("Authorization","dooray-api "+String(TOKEN));
  http.addHeader("Content-Type","application/json");
  int code=http.GET();
  if(code>0){ body=http.getString(); }
  http.end(); return code==200;
}

/* ----- Dooray API ----- */
bool fetchToday(JsonDocument& doc){
  String url="https://api.dooray.com/reservation/v1/resource-reservations?";
  url+="resourceIds="+String(RESOURCE_IDS);
  url+="&timeMin="+isoDate("00:00:00");
  url+="&timeMax="+isoDate("23:59:59");
  String body; if(!httpsGET(url,body)) return false;
  DeserializationError err=deserializeJson(doc, body);
  return !err;
}

bool postReservation(const char* resId,const String& startISO,const String& endISO){
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http; http.begin(client,"https://api.dooray.com/reservation/v1/resource-reservations");
  http.addHeader("Authorization","dooray-api "+String(TOKEN));
  http.addHeader("Content-Type","application/json");
  StaticJsonDocument<512> j; j["resourceId"]=resId; j["subject"]="즉석 예약";
  j["startedAt"]=startISO; j["endedAt"]=endISO; j["wholeDayFlag"]=false;
  String payload; serializeJson(j,payload);
  int code=http.POST(payload); http.end(); return code==200||code==201;
}

/* -------- UI helpers -------- */
void headerBar(){
  tft.fillRect(0,0,tft.width(),20,TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE,TFT_DARKGREEN);
  tft.drawString("TODAY",10,4,2);
  tft.drawString("RESERVE",120,4,2);
  int selX = currentPage==PAGE_TODAY?10:120;
  tft.fillRect(selX-2,18,60,2,TFT_YELLOW);
}

void pageToday(){
  StaticJsonDocument<6144> doc;
  if(!fetchToday(doc)){ tft.drawString("API ERR",0,30,2); return; }
  tft.fillRect(0,22,tft.width(),tft.height()-22,TFT_BLACK);
  tft.setTextColor(TFT_CYAN,TFT_BLACK);
  struct tm ti; getLocalTime(&ti);
  char d[32]; strftime(d,sizeof(d),"%Y-%m-%d (%a)",&ti);
  tft.drawString(d,0,24,2);
  JsonArray arr = doc["result"].as<JsonArray>();
  uint16_t y=44;
  tft.setTextColor(TFT_WHITE,TFT_BLACK);
  for(JsonObject r:arr){
    String room=r["resource"]["name"].as<String>();
    String s=String(r["startedAt"].as<const char*>()).substring(11,16);
    String e=String(r["endedAt"].as<const char*>()).substring(11,16);
    String subj=r["subject"].as<String>();
    tft.drawString(s+"-"+e+" "+room,0,y,2); y+=18;
    tft.drawString("  ▶ "+subj,0,y,2); y+=20;
    if(y>tft.height()-20) break;
  }
}

void pageReserve(){
  tft.fillRect(0,22,tft.width(),tft.height()-22,TFT_BLACK);
  tft.setTextColor(TFT_YELLOW,TFT_BLACK);
  tft.drawString("예약 버튼을 터치:",0,26,2);
  const char* labels[4]={"A-6F 소3","B-6F 소1","B-6F 소2","B-6F 중1"};
  int btnH=40;
  for(int i=0;i<4;i++){
    int y=50+i*btnH;
    tft.drawRect(10,y,tft.width()-20,btnH-5,TFT_WHITE);
    tft.drawString(labels[i],20,y+10,2);
  }
}

/* touch dispatch */
void handleTouch(){
  if(!isTouched()) return;
  TS_Point p = ts.getPoint();
  int x = map(p.x,CALIB_X_MAX,CALIB_X_MIN,0,tft.width());
  int y = map(p.y,CALIB_Y_MAX,CALIB_Y_MIN,0,tft.height());

  // header taps
  if(y<20){
    currentPage = (x<100)?PAGE_TODAY:PAGE_RESERVE;
    headerBar();
    currentPage==PAGE_TODAY ? pageToday() : pageReserve();
    return;
  }

  if(currentPage==PAGE_RESERVE && y>50){
    int index=(y-50)/40;
    if(index>=0 && index<4){
      const char* ids[4]={"3868297860122914233","3868312518617103681","3868312634809468080","3868312748489680534"};
      time_t now=time(nullptr)+60; // 1분 후 시작
      char isoS[32], isoE[32];
      struct tm t; localtime_r(&now,&t);
      strftime(isoS,sizeof(isoS),"%Y-%m-%dT%H:%M:%S+09:00",&t);
      t.tm_min+=30; mktime(&t); // 30분 예약
      strftime(isoE,sizeof(isoE),"%Y-%m-%dT%H:%M:%S+09:00",&t);
      bool ok=postReservation(ids[index],isoS,isoE);
      tft.setTextColor(ok?TFT_GREEN:TFT_RED,TFT_BLACK);
      tft.drawString(ok?"예약 성공":"예약 실패",20,220,2);
    }
  }
}

void setup(){
  Serial.begin(115200);
  pinMode(TFT_BL,OUTPUT); digitalWrite(TFT_BL,HIGH);
  tft.init(); tft.setRotation(1);
  headerBar(); tft.setCursor(0,24);

  ts.begin(); ts.setRotation(1);

  WiFi.begin(WIFI_SSID,WIFI_PASS);
  while(WiFi.status()!=WL_CONNECTED){ delay(300); }

  configTime(9*3600,0,"pool.ntp.org");
  while(time(nullptr)<1700000000) delay(100); // wait sync

  pageToday();
}

void loop(){
  handleTouch();
  if(millis()-lastRefresh>600000 && currentPage==PAGE_TODAY){
    lastRefresh=millis(); pageToday();
  }
}
