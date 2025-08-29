// ============================================================
// ESL 296x128 BWR — Dooray 4 Rooms (최종 안정화)
// - 변경 감지 후 선택적 업데이트
// - Partition: No OTA (2MB APP/2MB SPIFFS)
// ============================================================

#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>
#include <Preferences.h>

// ---------- Wi-Fi / HTTP / JSON / Time ----------
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_sntp.h>
#include <esp_task_wdt.h>
extern "C" {
  #include "esp_bt.h"
  #include "esp_wifi.h"
  #include "esp_bt_main.h"
}

// ---------- BLE ----------
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <BLERemoteCharacteristic.h>

// ---------- Canvas & Font ----------
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>

// ---------- MD5 ----------
#include <MD5Builder.h>

// ------------------- User config -------------------
// Wi-Fi
static const char* WIFI_SSID = "Smart Global";
static const char* WIFI_PASS = "";
// Dooray 개인 API 토큰 (필수)
static const char* DOORAY_TOKEN = "s701wolho5si:IvK8-thPTzGbr8DiA2bZ8Q";
static const char* DOORAY_BASE  = "https://api.dooray.com";

// 업데이트 간격 (분)
static const int UPDATE_INTERVAL_MIN = 10;

// ESL 태그 정보
struct Tag { 
  const char* mac; 
  const char* resId; 
  const char* label;
  char lastHash[33];
  uint32_t lastUpdate;  // time_t를 uint32_t로 변경
  bool needsUpdate;
};

static Tag TAGS[] = {
  { "FF:FF:92:95:75:78", "3868297860122914233", "소회의실3", "", 0, false },
  { "FF:FF:92:95:73:06", "3868312518617103681", "소회의실1", "", 0, false },
  { "FF:FF:92:95:73:04", "3868312634809468080", "소회의실2", "", 0, false },
  { "FF:FF:92:95:95:81", "3868312748489680534", "중회의실1", "", 0, false },
};
static constexpr int NUM_TAGS = sizeof(TAGS)/sizeof(TAGS[0]);

// ------------------- Canvas -------------------
static constexpr int DRAW_W = 296, DRAW_H = 128;
static GFXcanvas1* canvasBW = nullptr;   
static GFXcanvas1* canvasRED = nullptr;  
static U8G2_FOR_ADAFRUIT_GFX u8g2;

static const bool MIRROR_X         = true;
static const bool BW_ONE_IS_WHITE  = true;   
static const bool RED_ONE_IS_WHITE = false;  

// ------------------- Data Types -------------------
struct Resv { 
  char subj[48]; 
  char who[24]; 
  uint8_t startH, startM, endH, endM;
  time_t st=0, ed=0; 
};

struct RoomData { 
  Resv list[12];
  uint8_t count=0; 
  bool ok=false;
  char dataHash[33];
};
static RoomData g_room[NUM_TAGS];

static Preferences prefs;

// ------------------- Canvas Management -------------------
static void initCanvas() {
  if(!canvasBW) canvasBW = new GFXcanvas1(DRAW_W, DRAW_H);
  if(!canvasRED) canvasRED = new GFXcanvas1(DRAW_W, DRAW_H);
}

static void freeCanvas() {
  if(canvasBW) { delete canvasBW; canvasBW = nullptr; }
  if(canvasRED) { delete canvasRED; canvasRED = nullptr; }
}

// ------------------- Hash 생성 -------------------
static void generateDataHash(const RoomData& rd, char* hash) {
  MD5Builder md5;
  md5.begin();
  
  char buffer[256];
  for(int i = 0; i < rd.count; i++) {
    snprintf(buffer, sizeof(buffer), "%02d%02d%02d%02d%s%s", 
             rd.list[i].startH, rd.list[i].startM,
             rd.list[i].endH, rd.list[i].endM,
             rd.list[i].who, rd.list[i].subj);
    md5.add((uint8_t*)buffer, strlen(buffer));
  }
  
  md5.calculate();
  strcpy(hash, md5.toString().c_str());
}

// ------------------- NVS 저장/로드 (수정됨) -------------------
static void saveTagState(int idx) {
  char key[16];
  char keyTime[16];
  snprintf(key, sizeof(key), "tag%d", idx);
  snprintf(keyTime, sizeof(keyTime), "tag%dt", idx);
  
  prefs.begin("esl-dooray", false);
  prefs.putString(key, TAGS[idx].lastHash);
  prefs.putULong(keyTime, TAGS[idx].lastUpdate);  // 별도 키 사용
  prefs.end();
}

static void loadTagStates() {
  prefs.begin("esl-dooray", true);
  
  for(int i = 0; i < NUM_TAGS; i++) {
    char key[16];
    char keyTime[16];
    snprintf(key, sizeof(key), "tag%d", i);
    snprintf(keyTime, sizeof(keyTime), "tag%dt", i);
    
    String hash = prefs.getString(key, "");
    strcpy(TAGS[i].lastHash, hash.c_str());
    TAGS[i].lastUpdate = prefs.getULong(keyTime, 0);  // 별도 키 사용
  }
  
  prefs.end();
}

// ------------------- Time -------------------
static inline void setTZ_KST(){ setenv("TZ","KST-9",1); tzset(); }
static inline bool timeReady(){ return time(nullptr) > 1700000000; }

static void ensureTime(){
  if (timeReady()){ setTZ_KST(); return; }
  setTZ_KST();
  configTzTime("KST-9","kr.pool.ntp.org","time.google.com");
  struct tm ti; 
  for(int i=0;i<20;i++) {
    if (getLocalTime(&ti,500)){ 
      setTZ_KST(); 
      return; 
    }
    delay(100);
  }
}

// ------------------- Utils -------------------
static void urlEncode(const char* s, char* out, size_t maxLen){
  const char* hex="0123456789ABCDEF";
  size_t j = 0;
  for (size_t i=0; s[i] && j<maxLen-4; ++i){
    unsigned char c=(unsigned char)s[i];
    if (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~'){
      out[j++] = c;
    } else {
      out[j++] = '%';
      out[j++] = hex[(c>>4)&0xF];
      out[j++] = hex[c&0xF];
    }
  }
  out[j] = '\0';
}

static time_t parseISO(const char* iso, uint8_t& h, uint8_t& m){
  if(!iso || strlen(iso) < 19) return 0;
  
  struct tm tmv = {};
  sscanf(iso, "%d-%d-%dT%d:%d:%d",
         &tmv.tm_year, &tmv.tm_mon, &tmv.tm_mday,
         &tmv.tm_hour, &tmv.tm_min, &tmv.tm_sec);
  
  h = tmv.tm_hour;
  m = tmv.tm_min;
  
  tmv.tm_year -= 1900;
  tmv.tm_mon -= 1;
  
  return mktime(&tmv);
}

static void kstTodayISO(char* tMin, char* tMax){
  time_t now=time(nullptr); 
  struct tm lt; 
  localtime_r(&now,&lt);
  lt.tm_hour=0; lt.tm_min=0; lt.tm_sec=0; 
  
  strftime(tMin, 40, "%Y-%m-%dT%H:%M:%S+09:00", &lt);
  
  time_t end=mktime(&lt)+24*3600;
  localtime_r(&end,&lt); 
  strftime(tMax, 40, "%Y-%m-%dT%H:%M:%S+09:00", &lt);
}

// ------------------- Dooray fetch -------------------
static bool fetchReservationsToday(const char* resourceId, RoomData& rd){
  rd.count = 0;
  rd.ok = false;
  
  char tMin[40], tMax[40];
  kstTodayISO(tMin, tMax);
  
  char encodedId[128], encodedMin[128], encodedMax[128];
  urlEncode(resourceId, encodedId, sizeof(encodedId));
  urlEncode(tMin, encodedMin, sizeof(encodedMin));
  urlEncode(tMax, encodedMax, sizeof(encodedMax));
  
  char url[512];
  snprintf(url, sizeof(url), "%s/reservation/v1/resource-reservations?resourceIds=%s&timeMin=%s&timeMax=%s",
           DOORAY_BASE, encodedId, encodedMin, encodedMax);

  WiFiClientSecure cli;
  cli.setInsecure();
  
  HTTPClient http;
  http.setConnectTimeout(8000); 
  http.setTimeout(12000);
  http.setReuse(false);
  
  char authHdr[256];
  snprintf(authHdr, sizeof(authHdr), "dooray-api %s", DOORAY_TOKEN);
  http.addHeader("Authorization", authHdr);
  http.addHeader("Accept","application/json");
  
  if (!http.begin(cli, url)){ 
    Serial.println("[Dooray] http.begin fail"); 
    return false; 
  }

  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  
  Serial.printf("[Dooray] HTTP %d, %d bytes\n", code, body.length());
  if (code != 200 || body.length() == 0) return false;

  // JSON 파싱
  StaticJsonDocument<512> filter;
  filter["result"][0]["subject"] = true;
  filter["result"][0]["startedAt"] = true;
  filter["result"][0]["endedAt"] = true;
  filter["result"][0]["users"]["from"]["member"]["name"] = true;
  filter["result"][0]["users"]["from"]["emailUser"]["name"] = true;

  StaticJsonDocument<4096> doc;
  DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  
  if (e){ 
    Serial.printf("[JSON] Error: %s\n", e.c_str()); 
    return false; 
  }
  
  JsonArray arr = doc["result"].as<JsonArray>();
  if (arr.isNull()) return false;

  for (JsonVariant v : arr){
    if (rd.count >= 12) break;
    
    Resv& r = rd.list[rd.count];
    
    const char* subj = v["subject"] | "(제목 없음)";
    strncpy(r.subj, subj, sizeof(r.subj)-1);
    
    const char* start = v["startedAt"] | "";
    const char* end = v["endedAt"] | v["EndedAt"] | "";
    
    r.st = parseISO(start, r.startH, r.startM);
    r.ed = parseISO(end, r.endH, r.endM);
    
    const char* who = v["users"]["from"]["member"]["name"] | 
                      v["users"]["from"]["emailUser"]["name"] | "예약자";
    strncpy(r.who, who, sizeof(r.who)-1);
    
    if (r.st > 0 && r.ed > r.st) rd.count++;
  }
  
  std::sort(rd.list, rd.list + rd.count, [](const Resv&a, const Resv&b){ 
    return a.st < b.st; 
  });
  
  rd.ok = true;
  generateDataHash(rd, rd.dataHash);
  
  return true;
}

static int getCurrentMeetingIdx(const RoomData& rd){
  if (!timeReady()) return -1;
  time_t now = time(nullptr);
  for(int i=0; i<rd.count; i++) {
    if (now >= rd.list[i].st && now < rd.list[i].ed) return i;
  }
  return -1;
}

// ------------------- 화면 렌더링 -------------------
static void drawScreen(const Tag& tag, const RoomData& rd){
  if(!canvasBW || !canvasRED) return;
  
  canvasBW->fillScreen(1);  
  canvasRED->fillScreen(1); 

  u8g2.setFont(u8g2_font_unifont_t_korean2);
  u8g2.setFontMode(1);

  const int topY = 20;
  const int marginL = 8, marginR = 8;
  
  // 1. 날짜/시간 표시 (12.25(금) - 14)
  u8g2.begin(*canvasBW);
  u8g2.setForegroundColor(0); 
  u8g2.setBackgroundColor(1);
  
  char dateStr[24];
  if (timeReady()) {
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    const char* weekdays[] = {"일", "월", "화", "수", "목", "금", "토"};
    snprintf(dateStr, sizeof(dateStr), "%d.%d(%s)%02d", 
             lt.tm_mon+1, lt.tm_mday, weekdays[lt.tm_wday], lt.tm_hour);
  } else {
    strcpy(dateStr, "--.--(-)--");
  }
  u8g2.drawUTF8(marginL, topY, dateStr);
  
  // 2. 회의실명 (중앙)
  int labelWidth = u8g2.getUTF8Width(tag.label);
  int labelX = (DRAW_W - labelWidth) / 2;
  u8g2.drawUTF8(labelX, topY, tag.label);
  
  // 3. 상태 표시 (우측)
  int currentIdx = getCurrentMeetingIdx(rd);
  bool busy = (currentIdx >= 0);
  const char* statusText = busy ? "회의중" : "예약가능";
  
  if (busy){
    // 빨간 배경
    u8g2.begin(*canvasRED);
    u8g2.setForegroundColor(1);
    int sw = u8g2.getUTF8Width(statusText);
    int bx = DRAW_W - marginR - sw - 14;
    int by = 6;
    canvasRED->fillRect(bx, by, sw + 14, 18, 0);
    u8g2.drawUTF8(bx + 7, topY, statusText);
  } else {
    // 검정 테두리
    u8g2.begin(*canvasBW);
    u8g2.setForegroundColor(0);
    int sw = u8g2.getUTF8Width(statusText);
    int bx = DRAW_W - marginR - sw - 14;
    // canvasBW->drawRect(bx, 6, sw + 14, 18, 0);
    u8g2.drawUTF8(bx + 7, topY, statusText);
  }

  // 구분선
  canvasBW->drawFastHLine(0, 30, DRAW_W, 0);
  // canvasBW->drawFastHLine(0, 31, DRAW_W, 0);

  // 예약 리스트
  const int y0 = 48;
  const int lineHeight = 16;
  const int listX = 6;
  
  if(rd.count == 0){
    u8g2.begin(*canvasBW);
    u8g2.setForegroundColor(0);
    const char* msg = "오늘 예약 없음";
    int msgW = u8g2.getUTF8Width(msg);
    u8g2.drawUTF8((DRAW_W - msgW)/2, 75, msg);
    
    const char* submsg = "예약 가능합니다";
    int subW = u8g2.getUTF8Width(submsg);
    u8g2.drawUTF8((DRAW_W - subW)/2, 95, submsg);
  } else {
    int shown = (rd.count > 6) ? 6 : rd.count;
    
    for(int i=0; i<shown; i++){
      int y = y0 + i*lineHeight;
      bool isNow = (i == currentIdx);
      
      char timeStr[14];
      snprintf(timeStr, sizeof(timeStr), "%02d:%02d-%02d:%02d", 
               rd.list[i].startH, rd.list[i].startM,
               rd.list[i].endH, rd.list[i].endM);
      
      // 이름 - 8자 제한
      char who[36];
      strncpy(who, rd.list[i].who, 35);
      who[35] = '\0';
      if(strlen(who) > 35) {
        who[33] = '.';
        who[34] = '.';
        who[35] = '\0';
      }
      
      // 제목  
      char subj[120];
      strncpy(subj, rd.list[i].subj, 119);
      subj[119] = '\0';
      if(strlen(subj) > 119) {
        subj[116] = '.';
        subj[117] = '.';
        subj[118] = '\0';
      }    
      
      if (isNow) {
        // 현재 회의: 빨간 배경
        canvasRED->fillRect(listX-2, y-13, DRAW_W-listX-2, 16, 0);
        
        u8g2.begin(*canvasRED);
        u8g2.setForegroundColor(1);
        
        char fullLine[80];
        snprintf(fullLine, sizeof(fullLine), "%s %s %s", timeStr, who, subj);
        u8g2.drawUTF8(listX+2, y, fullLine);
        
      } else {
        u8g2.begin(*canvasBW);
        u8g2.setForegroundColor(0);
        
        char fullLine[80];
        snprintf(fullLine, sizeof(fullLine), "%s %s %s", timeStr, who, subj);
        u8g2.drawUTF8(listX+2, y, fullLine);
      }
    }
    
    if(rd.count > 6) {
      u8g2.begin(*canvasBW);
      u8g2.setForegroundColor(0);
      char moreStr[16];
      snprintf(moreStr, sizeof(moreStr), "... 외 %d건", rd.count-6);
      u8g2.drawUTF8(listX+2, y0 + 6*lineHeight, moreStr);
    }
  }

  if (!rd.ok){
    u8g2.begin(*canvasBW);
    u8g2.setForegroundColor(0);
    u8g2.drawUTF8(8, 115, "※ 네트워크 오류");
  }

  canvasBW->drawRect(0, 0, DRAW_W, DRAW_H, 0);
}

// ------------------- Build payload -------------------
static std::vector<uint8_t> g_payload;

static void buildPayload(){
  if(!canvasBW || !canvasRED) return;
  
  g_payload.clear();
  g_payload.reserve(9472);
  
  const uint8_t* bw  = canvasBW->getBuffer();
  const uint8_t* red = canvasRED->getBuffer();
  const int bpr = (DRAW_W+7)/8;

  auto getBit=[&](const uint8_t* buf,int x,int y)->bool{
    int idx=y*bpr + (x>>3); 
    uint8_t mask=(0x80>>(x&7)); 
    return (buf[idx]&mask)!=0;
  };
  
  auto pack=[&](const uint8_t* buf,bool ONE_IS_WHITE){
    for(int xx=0; xx<DRAW_W; ++xx){
      int x = MIRROR_X ? (DRAW_W-1-xx) : xx;
      for(int byteIdx=0; byteIdx<16; ++byteIdx){
        uint8_t b=0;
        for(int bit=0; bit<8; ++bit){
          int y=byteIdx*8+bit; 
          if (y>=DRAW_H) continue;
          bool one=getBit(buf,x,y);
          uint8_t eslBit = ONE_IS_WHITE ? (one?1:0) : (one?0:1);
          if (eslBit) b |= (1<<(7-bit));
        }
        g_payload.push_back(b);
      }
    }
  };
  
  pack(bw , BW_ONE_IS_WHITE);
  pack(red, RED_ONE_IS_WHITE);
  Serial.printf("[PAYLOAD] %u bytes\n",(unsigned)g_payload.size());
}

// ------------------- BLE uploader -------------------
static BLEUUID SVC_UUID((uint16_t)0xFEF0);
static BLEUUID CMD_UUID((uint16_t)0xFEF1);
static BLEUUID IMG_UUID((uint16_t)0xFEF2);

static BLEClient* g_cli=nullptr;
static BLERemoteCharacteristic* g_cmd=nullptr;
static BLERemoteCharacteristic* g_img=nullptr;

static volatile bool g_got01=false, g_got02=false, g_got05=false;
static volatile uint8_t g_st05=0; 
static volatile bool g_hasAck=false; 
static volatile uint32_t g_ack=0;
static uint16_t g_partMsgSize=244; 
static size_t g_partDataSize=240;
static std::vector<uint8_t> g_lastChunk; 
static uint32_t g_lastTxMs=0;

static void onNotifyCMD(BLERemoteCharacteristic* c, uint8_t* p, size_t n, bool){
  if (!p || !n) return;
  uint8_t op=p[0];
  if(op==0x01 && n>=3){
    g_partMsgSize = (uint16_t)(p[1] | (p[2]<<8));
    g_partDataSize = (g_partMsgSize>=4)?(g_partMsgSize-4):240;
    g_got01=true;
  } else if(op==0x02){
    g_got02=true;
  } else if(op==0x05){
    g_got05=true; 
    g_st05=(n>=2)?p[1]:0xFF; 
    g_hasAck=(n>=6);
    if(g_hasAck) g_ack=(uint32_t)p[2]|((uint32_t)p[3]<<8)|((uint32_t)p[4]<<16)|((uint32_t)p[5]<<24);
  }
}

static void enableCCCDNotify(BLERemoteCharacteristic* c){
  auto d=c->getDescriptor(BLEUUID((uint16_t)0x2902));
  if(!d) return;
  uint8_t v[2]={0x01,0x00};
  d->writeValue(v,2,true);
  delay(100);
}

static bool connectAndDiscover(const Tag& t){
  if(g_cli) {
    if(g_cli->isConnected()) g_cli->disconnect();
    delete g_cli;
    g_cli = nullptr;
    delay(500);
  }
  
  BLEAddress addr(t.mac);
  g_cli = BLEDevice::createClient();
  // setConnectTimeout 제거 (ESP32 Arduino 2.0.17에서 지원 안함)
  
  bool connected = false;
  for(int retry = 0; retry < 3; retry++) {
    Serial.printf("[BLE] Try %d/3\n", retry+1);
    if(g_cli->connect(addr)) {
      connected = true;
      break;
    }
    delay(1000);
    esp_task_wdt_reset();
  }
  
  if(!connected) {
    Serial.println("[BLE] Failed");
    return false;
  }
  
  delay(500);
  
  auto svc=g_cli->getService(SVC_UUID);
  if(!svc) return false;
  
  g_cmd=svc->getCharacteristic(CMD_UUID);
  g_img=svc->getCharacteristic(IMG_UUID);
  if(!g_cmd||!g_img) return false;
  
  g_cmd->registerForNotify(onNotifyCMD);
  enableCCCDNotify(g_cmd);
  delay(200);
  
  return true;
}

static void disconnectAndCleanup(){
  if(g_cli){ 
    if(g_cli->isConnected()) g_cli->disconnect(); 
    delete g_cli; 
    g_cli = nullptr;
  }
  g_cmd = nullptr; 
  g_img = nullptr;
  delay(500);
}

static void le32(uint8_t* p, uint32_t v){ 
  p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; 
}

static void sendPartA(uint32_t part){
  size_t total=g_payload.size();
  size_t dataSz = std::min((size_t)g_partDataSize, (size_t)240);
  size_t off=part * dataSz;
  if(off>=total) return;
  
  size_t n=std::min(dataSz, total-off);
  g_lastChunk.resize(4+n);
  le32(g_lastChunk.data(),part);
  memcpy(&g_lastChunk[4],&g_payload[off],n);
  
  if(g_img) g_img->writeValue(g_lastChunk.data(), g_lastChunk.size(), false);
  g_lastTxMs=millis();
  
  if((part%20)==0){ 
    uint32_t parts=(total+dataSz-1)/dataSz; 
    Serial.printf("[TX] %u/%u\n",part,parts); 
  }
}

static bool tryProtocolA(){
  g_got01=g_got02=g_got05=false; 
  g_hasAck=false; 
  g_partMsgSize=244; 
  g_partDataSize=240; 
  g_lastChunk.clear();
  
  uint8_t c1=0x01; 
  if(g_cmd) g_cmd->writeValue(&c1,1,true);
  
  uint32_t t0=millis();
  while (millis()-t0<3000){
    if (g_got01) break;
    if (!g_cli->isConnected()) return false;
    delay(10);
    esp_task_wdt_reset();
  }
  
  if (!g_got01) return false;

  g_got01=false;
  uint32_t total=g_payload.size();
  uint8_t c2[8]={0x02,(uint8_t)(total&0xFF),(uint8_t)((total>>8)&0xFF),
                 (uint8_t)((total>>16)&0xFF),(uint8_t)((total>>24)&0xFF),0,0,0};
  if(g_cmd) g_cmd->writeValue(c2,sizeof(c2),false);
  
  delay(100);
  
  uint8_t c3=0x03; 
  if(g_cmd) g_cmd->writeValue(&c3,1,false);

  size_t dataSz = std::min((size_t)g_partDataSize, (size_t)240);
  uint32_t parts=(g_payload.size()+dataSz-1)/dataSz;
  sendPartA(0);

  uint32_t lastAckPart = UINT32_MAX;
  uint32_t stallCount = 0;
  
  while (true){
    if (!g_cli->isConnected()) return false;
    
    if (g_got05){
      g_got05=false;
      stallCount = 0;
      
      if(g_st05==0x08) return true;
      else if(g_st05!=0x00) return false;
      else if(g_hasAck){
        uint32_t ack=g_ack;
        if (ack >= parts) return true;
        if (ack != lastAckPart){
          lastAckPart = ack;
          sendPartA(ack);
        }
      }
    }
    
    if (!g_lastChunk.empty() && millis()-g_lastTxMs > 1500){
      if(g_img) g_img->writeValue(g_lastChunk.data(), g_lastChunk.size(), false);
      g_lastTxMs=millis();
      stallCount++;
      
      if(stallCount > 20) return false;
    }
    
    if (millis()-t0 > 60000) return false;
    
    delay(5);
    esp_task_wdt_reset();
  }
}

// ------------------- Main -------------------
static void wifiOnAndConnect(){
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000){ 
    delay(200); 
    esp_task_wdt_reset();
  }
  
  if(WiFi.status()==WL_CONNECTED) {
    Serial.printf("[WiFi] Connected: %s\n", WiFi.localIP().toString().c_str());
  }
}

static void wifiOff(){
  WiFi.disconnect(true, true);
  delay(500);
  esp_wifi_stop();
  delay(500);
  esp_wifi_deinit();
  delay(500);
  WiFi.mode(WIFI_OFF);
  delay(1500);
}

static void processUpdates(bool forceUpdate = false) {
  Serial.println("\n═══ Update Process ═══");
  
  initCanvas();
  
  wifiOnAndConnect();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[ERROR] WiFi failed");
    wifiOff();
    freeCanvas();
    return;
  }
  
  ensureTime();
  uint32_t now = (uint32_t)time(nullptr);  // time_t를 uint32_t로 캐스트
  
  // 데이터 가져오기
  int updateCount = 0;
  for(int i = 0; i < NUM_TAGS; i++) {
    Serial.printf("[%d/%d] %s... ", i+1, NUM_TAGS, TAGS[i].label);
    
    bool fetchOk = fetchReservationsToday(TAGS[i].resId, g_room[i]);
    if (!fetchOk) {
      Serial.println("FAIL");
      continue;
    }
    
    bool dataChanged = (strcmp(g_room[i].dataHash, TAGS[i].lastHash) != 0);
    bool timeExpired = (now - TAGS[i].lastUpdate) > (UPDATE_INTERVAL_MIN * 60);
    
    if (forceUpdate || dataChanged || timeExpired) {
      TAGS[i].needsUpdate = true;
      updateCount++;
      Serial.println("UPDATE");
    } else {
      TAGS[i].needsUpdate = false;
      Serial.println("SKIP");
    }
    
    esp_task_wdt_reset();
  }
  
  wifiOff();
  delay(2000);
  
  // 업데이트 실행
  if (updateCount > 0) {
    Serial.printf("\n[INFO] Updating %d tags\n", updateCount);
    
    BLEDevice::init("ESL");
    delay(1000);
    
    for(int i = 0; i < NUM_TAGS; i++) {
      if (!TAGS[i].needsUpdate) continue;
      
      Serial.printf("\n[Update] %s... ", TAGS[i].label);
      
      drawScreen(TAGS[i], g_room[i]);
      buildPayload();
      
      bool uploadOk = false;
      for(int attempt = 0; attempt < 2 && !uploadOk; ++attempt) {
        if (!connectAndDiscover(TAGS[i])) {
          disconnectAndCleanup(); 
          delay(1500); 
          continue;
        }
        
        uploadOk = tryProtocolA();
        disconnectAndCleanup();
        
        if (!uploadOk && attempt < 1) { 
          delay(2000); 
        }
      }
      
      if (uploadOk) {
        strcpy(TAGS[i].lastHash, g_room[i].dataHash);
        TAGS[i].lastUpdate = now;
        saveTagState(i);
        Serial.println("OK");
      } else {
        Serial.println("FAIL");
      }
      
      delay(2000);
      esp_task_wdt_reset();
    }
    
    BLEDevice::deinit();
  } else {
    Serial.println("\n[INFO] No updates needed");
  }
  
  freeCanvas();
  Serial.printf("[MEM] Free: %d bytes\n", ESP.getFreeHeap());
}

void setup(){
  Serial.begin(115200);
  delay(2000);
  
  esp_task_wdt_init(30, true);
  esp_task_wdt_add(NULL);
  
  if(esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
    esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);
  }

  Serial.println("\n╔════════════════════════════════════╗");
  Serial.println("║   ESL Dooray Final v3.0           ║");
  Serial.println("║   2MB APP / 2MB SPIFFS             ║");
  Serial.println("╚════════════════════════════════════╝");
  
  loadTagStates();
  
  bool isFirstRun = (TAGS[0].lastUpdate == 0);
  processUpdates(isFirstRun);
}

void loop() {
  static unsigned long lastCheck = 0;
  unsigned long now = millis();
  
  if (now - lastCheck > UPDATE_INTERVAL_MIN * 60 * 1000) {
    lastCheck = now;
    processUpdates(false);
  }
  
  delay(10000);
  esp_task_wdt_reset();
}
