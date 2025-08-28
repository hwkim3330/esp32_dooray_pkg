// ============================================================
// ESL 296x128 BWR — Dooray 4 Rooms (Wi-Fi→OFF→BLE, v3.9 dual-proto)
//  - Step: Wi-Fi fetch all → Wi-Fi OFF → BLE upload all
//  - Tries Protocol A (0x01/0x02/0x03/0x05). If no reply, falls back to
//    Protocol B (0x75 raw start → payload → 0x76 commit).
//  - CCCD = notify+indicate (0x0003) on CMD/IMG.
//  - Fill WIFI_SSID / WIFI_PASS / DOORAY_TOKEN
// ============================================================

#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cctype>

// ---------- Wi-Fi / HTTP / JSON / Time ----------
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_sntp.h>
extern "C" {
  #include "esp_bt.h"
  #include "esp_wifi.h"
}

// ---------- BLE (ESP32 BLE Arduino) -------------
#include <BLEDevice.h>
#include <BLEUtils.h>
#include <BLEClient.h>
#include <BLEAddress.h>
#include <BLERemoteCharacteristic.h>

// ---------- Canvas & Font ------------------------
#include <Adafruit_GFX.h>
#include <U8g2_for_Adafruit_GFX.h>

// ------------------- User config -------------------
static const char* WIFI_SSID  = "YOUR_SSID";
static const char* WIFI_PASS  = "YOUR_PASS";

// ★ Dooray 개인 토큰만 넣으면 됩니다.
static const char* DOORAY_TOKEN = "YOUR_DOORAY_TOKEN";

// 민간 클라우드 기본
static const char* DOORAY_BASE  = "https://api.dooray.com";

// Dooray resourceId + ESL MAC + 라벨(표시용)
struct Tag { const char* mac; const char* resId; const char* label; };
static Tag TAGS[] = {
  { "FF:FF:92:95:75:78", "3868297860122914233", "A동-6층 소회의실3" },
  { "FF:FF:92:95:73:06", "3868312518617103681", "B동-6층 소회의실1" },
  { "FF:FF:92:95:73:04", "3868312634809468080", "B동-6층 소회의실2" },
  { "FF:FF:92:95:95:81", "3868312748489680534", "B동-6층 중회의실1" },
};
static constexpr int NUM_TAGS = sizeof(TAGS)/sizeof(TAGS[0]);

// ------------------- Canvas -------------------
static constexpr int DRAW_W = 296;
static constexpr int DRAW_H = 128;
GFXcanvas1 canvasBW (DRAW_W, DRAW_H);   // black/white layer (1bit)
GFXcanvas1 canvasRED(DRAW_W, DRAW_H);   // red/white   layer (1bit)
U8G2_FOR_ADAFRUIT_GFX u8g2;

// 하드웨어 방향/비트 매핑
static const bool MIRROR_X         = true;
static const bool BW_ONE_IS_WHITE  = true;   // 1=white
static const bool RED_ONE_IS_WHITE = false;  // 1=white, 0=red-ink

// ------------------- Data Types -------------------
struct Resv { String subj, startISO, endISO, who; time_t st=0, ed=0; };
struct RoomData { Resv list[32]; int count=0; bool ok=false; };
static RoomData g_room[NUM_TAGS];

// ------------------- Time (KST) -------------------
static inline void setTZ_KST(){ setenv("TZ","KST-9",1); tzset(); }
static inline bool timeReady(){ return time(nullptr) > 1700000000; }

static void ensureTime(){
  if (timeReady()){ setTZ_KST(); return; }
  setTZ_KST();
  configTzTime("KST-9","kr.pool.ntp.org","time.google.com","time.cloudflare.com");
  struct tm ti; for(int i=0;i<40;i++) if (getLocalTime(&ti,500)){ setTZ_KST(); return; }

  // HTTP Date 폴백
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setConnectTimeout(15000); http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  if (!http.begin(cli, String(DOORAY_BASE)+"/")) return;
  const char* keys[]={"Date"}; http.collectHeaders(keys,1);
  int code=http.GET();
  if (code>0){
    String date=http.header("Date");
    if (date.length()>=10){
      int d,Y,h,m,s; char Mon[4];
      if (sscanf(date.c_str(),"%*3s, %d %3s %d %d:%d:%d GMT",&d,Mon,&Y,&h,&m,&s)==6){
        const char* tbl="JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* p=strstr(tbl,Mon);
        if (p){
          int mon=(p-tbl)/3+1;
          struct tm tmv={};
          tmv.tm_year=Y-1900; tmv.tm_mon=mon-1; tmv.tm_mday=d;
          tmv.tm_hour=h; tmv.tm_min=m; tmv.tm_sec=s;
          char* old=getenv("TZ"); setenv("TZ","UTC0",1); tzset();
          time_t gmt=mktime(&tmv);
          if (old) setenv("TZ",old,1); else unsetenv("TZ"); tzset();
          if (gmt>0){ struct timeval tv={gmt,0}; settimeofday(&tv,nullptr); setTZ_KST(); }
        }
      }
    }
  }
  http.end();
}

// ------------------- Utils -------------------
static String urlEncode(const String& s){
  String o; o.reserve(s.length()*3);
  const char* hex="0123456789ABCDEF";
  for (size_t i=0;i<s.length();++i){
    unsigned char c=(unsigned char)s[i];
    bool unres = (isalnum(c) || c=='-'||c=='_'||c=='.'||c=='~');
    if (unres) o+=char(c);
    else { o+='%'; o+=hex[(c>>4)&0xF]; o+=hex[c&0xF]; }
  }
  return o;
}
static String hhmm(const String& iso){
  int t=iso.indexOf('T'); if (t<0) return "--:--";
  return iso.substring(t+1,t+3)+":"+iso.substring(t+4,t+6);
}
static String authHeader(){ return String("dooray-api ")+DOORAY_TOKEN; }

static time_t mktime_utc(struct tm* tmv){
  char* old=getenv("TZ"); setenv("TZ","UTC0",1); tzset();
  time_t t=mktime(tmv);
  if (old) setenv("TZ",old,1); else unsetenv("TZ"); tzset();
  return t;
}
static bool isoToEpochUTC(String s, time_t& out){
  s.trim(); if (s.isEmpty()) return false;
  int dot=s.indexOf('.'); if(dot>=0){
    int tz=s.indexOf('Z',dot); if(tz<0){ int p1=s.indexOf('+',dot), p2=s.indexOf('-',dot); tz=(p1>=0)?p1:p2; }
    if (tz>dot) s.remove(dot, tz-dot); else s.remove(dot);
  }
  bool isZ = s.endsWith("Z"); if (isZ) s.remove(s.length()-1);
  int ofs=-1; for(int i=s.length()-6;i>=0 && i<(int)s.length();--i){ char c=s[i]; if(c=='+'||c=='-'){ ofs=i; break; } }
  int Y=s.substring(0,4).toInt(), M=s.substring(5,7).toInt(), D=s.substring(8,10).toInt();
  int h=s.substring(11,13).toInt(), m=s.substring(14,16).toInt();
  int sec=(s.length()>=19)? s.substring(17,19).toInt():0;
  int off=0;
  if(!isZ && ofs>0){
    char sign=s.charAt(ofs);
    int tzH=s.substring(ofs+1,ofs+3).toInt(), tzM=s.substring(ofs+4,ofs+6).toInt();
    off = tzH*3600+tzM*60; if (sign=='-') off=-off;
  }
  struct tm tmv={}; tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D; tmv.tm_hour=h; tmv.tm_min=m; tmv.tm_sec=sec;
  time_t base=mktime_utc(&tmv);
  out = isZ? base : (base - off);
  return out>0;
}
static void kstTodayISO(String& tMin,String& tMax){
  time_t now=time(nullptr); struct tm lt; localtime_r(&now,&lt);
  lt.tm_hour=0; lt.tm_min=0; lt.tm_sec=0; time_t start=mktime(&lt);
  time_t end=start+24*3600;
  char b1[40],b2[40]; strftime(b1,sizeof(b1),"%Y-%m-%dT%H:%M:%S+09:00",&lt);
  struct tm lt2; localtime_r(&end,&lt2); strftime(b2,sizeof(b2),"%Y-%m-%dT%H:%M:%S+09:00",&lt2);
  tMin=b1; tMax=b2;
}

// ------------------- Dooray fetch -------------------
static bool fetchReservationsToday(const char* resourceId, RoomData& rd){
  rd.count=0; rd.ok=false;
  String tMin,tMax; kstTodayISO(tMin,tMax);

  String url = String(DOORAY_BASE)+"/reservation/v1/resource-reservations"
             + "?resourceIds=" + urlEncode(resourceId)
             + "&timeMin="     + urlEncode(tMin)
             + "&timeMax="     + urlEncode(tMax);

  WiFiClientSecure cli; cli.setInsecure();
  cli.setHandshakeTimeout(14);
  HTTPClient http;
  http.setConnectTimeout(15000); http.setTimeout(20000);
  http.setReuse(false);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.useHTTP10(true);
  http.addHeader("Authorization",authHeader());
  http.addHeader("Accept","application/json");
  http.addHeader("Accept-Encoding","identity");
  http.addHeader("Connection","close");
  http.addHeader("User-Agent","ESP32-DOORAY/1.0");

  if (!http.begin(cli, url)){ Serial.println("[Dooray] http.begin fail"); return false; }

  int code=-1; String body;
  for (int tryi=0; tryi<3; ++tryi){
    code = http.GET();
    if (code==200){ body=http.getString(); if (body.length()>0) break; }
    delay(200*(tryi+1));
  }
  http.end();
  Serial.printf("[Dooray] HTTP %d, body %d bytes\n", code, body.length());
  if (code!=200 || body.length()==0) return false;

  // 메모리 절약 필터
  StaticJsonDocument<768> filter;
  JsonObject it = filter["result"].createNestedObject();
  it["subject"]=true; it["startedAt"]=true; it["endedAt"]=true; it["EndedAt"]=true;
  it["users"]["from"]["type"]=true;
  it["users"]["from"]["member"]["name"]=true;
  it["users"]["from"]["emailUser"]["name"]=true;
  it["users"]["from"]["emailUser"]["emailAddress"]=true;

  DynamicJsonDocument doc(16384);
  DeserializationError e = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (e){ Serial.printf("[Dooray] JSON %s\n", e.c_str()); return false; }
  JsonArray arr = doc["result"].as<JsonArray>(); if (arr.isNull()) return false;

  for (JsonVariant v : arr){
    if (rd.count >= (int)(sizeof(rd.list)/sizeof(rd.list[0]))) break;
    Resv r;
    r.subj     = (const char*)(v["subject"]  | "");
    if (r.subj.length()==0) r.subj="(제목 없음)";
    r.startISO = (const char*)(v["startedAt"]| "");
    r.endISO   = (const char*)(v["endedAt"]  | v["EndedAt"] | "");

    const char* t = v["users"]["from"]["type"] | "";
    String who;
    if (strcmp(t,"member")==0) who=(const char*)(v["users"]["from"]["member"]["name"] | "");
    else if (strcmp(t,"emailUser")==0){
      who=(const char*)(v["users"]["from"]["emailUser"]["name"] | "");
      if (who.length()==0) who=(const char*)(v["users"]["from"]["emailUser"]["emailAddress"] | "");
    }
    if (who.length()==0) who="예약자";
    r.who=who;

    isoToEpochUTC(r.startISO,r.st);
    isoToEpochUTC(r.endISO  ,r.ed);
    if (r.st>0 && r.ed>r.st) rd.list[rd.count++]=r;
  }
  std::sort(rd.list, rd.list+rd.count, [](const Resv&a,const Resv&b){ return a.st<b.st; });
  rd.ok = true;
  return true;
}

static bool timeBusyNow(const RoomData& rd){
  if (!timeReady()) return false;
  time_t now=time(nullptr);
  for(int i=0;i<rd.count;i++) if (now>=rd.list[i].st && now<rd.list[i].ed) return true;
  return false;
}

// ------------------- Render -------------------
static inline void drawUTF8_center(GFXcanvas1& dst, const char* text, int y){
  u8g2.begin(dst); u8g2.setFont(u8g2_font_unifont_t_korean2);
  int w=u8g2.getUTF8Width(text); int x=(DRAW_W-w)/2; u8g2.drawUTF8(x,y,text);
}

static void drawScreen(const Tag& tag, const RoomData& rd){
  canvasBW.fillScreen(1);  // white
  canvasRED.fillScreen(1); // white

  // 제목(검정)
  u8g2.begin(canvasBW);
  u8g2.setFont(u8g2_font_unifont_t_korean2);
  u8g2.setFontMode(1);
  u8g2.setForegroundColor(0); u8g2.setBackgroundColor(1);
  drawUTF8_center(canvasBW, tag.label, 20);

  // 상태
  if (timeBusyNow(rd)){
    const int W=120,H=22; int rx=(DRAW_W-W)/2, ry=32;
    for(int yy=ry; yy<ry+H; ++yy) for(int xx=rx; xx<rx+W; ++xx) canvasRED.drawPixel(xx,yy,0);
    u8g2.begin(canvasRED); u8g2.setForegroundColor(1);
    drawUTF8_center(canvasRED, "회의 중", ry+16);
  }else{
    u8g2.begin(canvasBW); u8g2.setForegroundColor(0); u8g2.setBackgroundColor(1);
    drawUTF8_center(canvasBW, "비어 있음", 48);
  }

  canvasBW.drawFastHLine(0,60,DRAW_W,0);

  // 예약 리스트(최대 5줄)
  int shown = std::min(rd.count, 5);
  const int y0=75, lh=18;
  time_t now=time(nullptr);
  for(int i=0;i<shown;i++){
    int y=y0+i*lh;
    bool on = timeReady() && now>=rd.list[i].st && now<rd.list[i].ed;
    if (on){ int cx=8, cy=y-6; for(int dy=-2;dy<=2;dy++) for(int dx=-2;dx<=2;dx++) if(dx*dx+dy*dy<=4) canvasRED.drawPixel(cx+dx,cy+dy,0); }
    String line = hhmm(rd.list[i].startISO) + "-" + hhmm(rd.list[i].endISO) + " " + rd.list[i].subj;
    u8g2.begin(canvasBW); u8g2.setForegroundColor(0); u8g2.setBackgroundColor(1);
    u8g2.drawUTF8(16,y,line.c_str());
  }

  if (!rd.ok){
    u8g2.begin(canvasBW); u8g2.setForegroundColor(0);
    u8g2.drawUTF8(10, 100, "네트워크 오류 / 데이터 없음");
  }

  canvasBW.drawRect(0,0,DRAW_W,DRAW_H,0);
}

// ------------------- Build payload (no header) -------------------
static std::vector<uint8_t> g_payload;

static void buildPayload(){
  g_payload.clear();
  const uint8_t* bw  = canvasBW.getBuffer();
  const uint8_t* red = canvasRED.getBuffer();
  const int bpr = (DRAW_W+7)/8; // bytes per row

  auto getBit=[&](const uint8_t* buf,int x,int y)->bool{
    int idx=y*bpr + (x>>3); uint8_t mask=(0x80>>(x&7)); return (buf[idx]&mask)!=0; // 1=white
  };
  auto pack=[&](const uint8_t* buf,bool ONE_IS_WHITE){
    for(int xx=0; xx<DRAW_W; ++xx){
      int x = MIRROR_X ? (DRAW_W-1-xx) : xx;
      for(int byteIdx=0; byteIdx<16; ++byteIdx){
        uint8_t b=0;
        for(int bit=0; bit<8; ++bit){
          int y=byteIdx*8+bit; if (y>=DRAW_H) continue;
          bool one=getBit(buf,x,y);                  // canvas: 1=white, 0=ink
          uint8_t eslBit = ONE_IS_WHITE ? (one?1:0) : (one?0:1);
          if (eslBit) b |= (1<<(7-bit));
        }
        g_payload.push_back(b);
      }
    }
  };
  pack(bw , BW_ONE_IS_WHITE);
  pack(red, RED_ONE_IS_WHITE);
  Serial.printf("[PAYLOAD] %u bytes (expect 9472)\n",(unsigned)g_payload.size());
}

// ------------------- BLE uploader -------------------
static BLEUUID SVC_UUID((uint16_t)0xFEF0);
static BLEUUID CMD_UUID((uint16_t)0xFEF1);
static BLEUUID IMG_UUID((uint16_t)0xFEF2);

BLEClient* g_cli=nullptr;
BLERemoteCharacteristic* g_cmd=nullptr;
BLERemoteCharacteristic* g_img=nullptr;

volatile bool g_got01=false, g_got02=false, g_got05=false;
volatile uint8_t g_st05=0; volatile bool g_hasAck=false; volatile uint32_t g_ack=0;
uint16_t g_partMsgSize=244; size_t g_partDataSize=240;
std::vector<uint8_t> g_lastChunk; uint32_t g_lastTxMs=0;

static void onNotifyGeneric(BLERemoteCharacteristic* c, uint8_t* p, size_t n, bool){
  Serial.print("[NTF ");
  Serial.print(c->getUUID().toString().c_str());
  Serial.print("] ");
  for(size_t i=0;i<n;i++){ char b[4]; sprintf(b,"%02X",p[i]); Serial.print(b); }
  Serial.println();

  if (!p || !n) return;
  uint8_t op=p[0];

  // Proto-A statuses
  if(op==0x01 && n>=3){
    g_partMsgSize = (uint16_t)(p[1] | (p[2]<<8));
    g_partDataSize = (g_partMsgSize>=4)?(g_partMsgSize-4):240;
    g_got01=true;
  } else if(op==0x02){
    g_got02=true;
  } else if(op==0x05){
    g_got05=true; g_st05=(n>=2)?p[1]:0xFF; g_hasAck=(n>=6);
    if(g_hasAck) g_ack=(uint32_t)p[2] | ((uint32_t)p[3]<<8) | ((uint32_t)p[4]<<16) | ((uint32_t)p[5]<<24);
  }
}

static bool enableCCCD(BLERemoteCharacteristic* c){
  auto d=c->getDescriptor(BLEUUID((uint16_t)0x2902)); if(!d){ Serial.println("[WARN] no CCCD"); return false; }
  // enable notify + indicate
  uint8_t v[2]={0x03,0x00};
  d->writeValue(v,2,true);
  delay(120);
  return true;
}

static inline void cmdWrite(const uint8_t* d,size_t n,bool resp){ if(g_cmd) g_cmd->writeValue((uint8_t*)d,n,resp); }
static inline void imgWrite(const uint8_t* d,size_t n,bool resp=false){ if(g_img) g_img->writeValue((uint8_t*)d,n,resp); }

static bool connectAndDiscover(const Tag& t){
  BLEAddress addr(t.mac);
  g_cli=BLEDevice::createClient();
  if(!g_cli->connect(addr)){ Serial.println("[BLE] connect fail"); return false; }

  // MTU
  g_cli->setMTU(247);
  delay(100);

  auto svc=g_cli->getService(SVC_UUID);
  if(!svc){ Serial.println("[BLE] svc missing"); return false; }
  g_cmd=svc->getCharacteristic(CMD_UUID);
  g_img=svc->getCharacteristic(IMG_UUID);
  if(!g_cmd||!g_img){ Serial.println("[BLE] char missing"); return false; }

  g_cmd->registerForNotify(onNotifyGeneric);
  g_img->registerForNotify(onNotifyGeneric);
  enableCCCD(g_cmd);
  enableCCCD(g_img);
  delay(150);
  return true;
}

static void disconnectAndCleanup(){
  if(g_cli){ if(g_cli->isConnected()) g_cli->disconnect(); delete g_cli; }
  g_cli=nullptr; g_cmd=nullptr; g_img=nullptr;
}

// ------------------- Protocol A (0x01/0x02/0x03/0x05) -------------------
static void le32(uint8_t* p, uint32_t v){ p[0]=v&0xFF; p[1]=(v>>8)&0xFF; p[2]=(v>>16)&0xFF; p[3]=(v>>24)&0xFF; }

static void sendPartA(uint32_t part){
  size_t total=g_payload.size();
  size_t dataSz = (size_t)std::min((size_t)g_partDataSize, (size_t)240);
  size_t off=(size_t)part * dataSz;
  if(off>=total) return;
  size_t n=std::min(dataSz, total-off);
  g_lastChunk.assign(4+n,0); le32(g_lastChunk.data(),part); memcpy(&g_lastChunk[4],&g_payload[off],n);
  imgWrite(g_lastChunk.data(), g_lastChunk.size(), false);
  g_lastTxMs=millis();
  if((part%10)==0){ uint32_t parts=(total+dataSz-1)/dataSz; Serial.printf("[A] Part %u/%u\n",part,parts); }
}

static bool tryProtocolA(){
  g_got01=g_got02=g_got05=false; g_hasAck=false; g_partMsgSize=244; g_partDataSize=240; g_lastChunk.clear();
  uint8_t c1=0x01; cmdWrite(&c1,1,true); Serial.println("[A] TX 0x01");
  uint32_t t0=millis();

  // 기다림: 2초 안에 0x01 응답 없으면 실패로 보고 빠져나감
  while (millis()-t0<2000){
    if (g_got01) break;
    if (!g_cli->isConnected()) return false;
    delay(10);
  }
  if (!g_got01){ Serial.println("[A] No reply to 0x01 → fallback"); return false; }

  g_got01=false;
  Serial.printf("[A] partMsg=%u data=%u\n",g_partMsgSize,g_partDataSize);

  uint32_t total=g_payload.size();
  uint8_t c2[8]={0x02,(uint8_t)(total&0xFF),(uint8_t)((total>>8)&0xFF),(uint8_t)((total>>16)&0xFF),(uint8_t)((total>>24)&0xFF),0x00,0x00,0x00};
  cmdWrite(c2,sizeof(c2),false); Serial.printf("[A] TX 0x02 len=%u\n", total);
  uint32_t t02=millis(); bool sent03=false;
  while (millis()-t02<800){
    if (g_got02){ break; }
    delay(10);
  }
  // 어떤 기기는 0x02 응답 없이 0x03을 요구
  uint8_t c3=0x03; cmdWrite(&c3,1,false); Serial.println("[A] TX 0x03"); sent03=true;

  // 전송
  uint32_t part=0;
  size_t dataSz = (size_t)std::min((size_t)g_partDataSize, (size_t)240);
  uint32_t parts=(g_payload.size()+dataSz-1)/dataSz;
  sendPartA(0);

  uint32_t lastAckPart = 0;
  while (true){
    if (!g_cli->isConnected()){ Serial.println("[A] disconnected"); return false; }
    if (g_got05){
      g_got05=false;
      if(g_st05==0x08){ Serial.println("[A] DONE"); return true; }
      else if(g_st05!=0x00){ Serial.printf("[A] ERR status=0x%02X\n", g_st05); return false; }
      else if(g_hasAck){
        uint32_t ack=g_ack;
        if (ack >= parts){ Serial.println("[A] ack>=parts → DONE"); return true; }
        if (ack != lastAckPart){
          lastAckPart = ack;
          sendPartA(ack);
        }
      }
    }
    // ACK 없으면 재전송
    if (millis()-g_lastTxMs > 1200){
      imgWrite(g_lastChunk.data(), g_lastChunk.size(), false);
      g_lastTxMs=millis();
    }
    // 안전 타임아웃
    if (millis()-t0 > 120000){ Serial.println("[A] timeout"); return false; }
    delay(1);
  }
}

// ------------------- Protocol B (0x75 RAW → payload → 0x76) -------------
static size_t safeChunkForRaw(){
  // GATT payload ≈ MTU-3. 보수적으로 120 바이트 제한.
  uint16_t mtu = g_cli ? g_cli->getMTU() : 23;
  size_t cap = (mtu>50)? (mtu-3) : 20;
  if (cap > 180) cap = 180;
  if (cap < 20)  cap = 20;
  return cap;
}

static bool tryProtocolB(){
  Serial.println("[B] Raw 0x75 start");
  uint8_t start = 0x75;
  if (!g_cmd->canWrite()){ Serial.println("[B] CMD not writable"); return false; }
  cmdWrite(&start,1,true);
  delay(50);

  size_t chunk = safeChunkForRaw();
  const uint8_t* p = g_payload.data();
  size_t total = g_payload.size();
  size_t off = 0;
  uint32_t t0=millis();
  while (off < total){
    size_t n = std::min(chunk, total-off);
    imgWrite(p+off, n, false);
    off += n;
    if ((off/chunk)%8==0){ Serial.printf("[B] %u/%u\n",(unsigned)off,(unsigned)total); }
    delay(3); // 살짝 템포
  }

  uint8_t commit = 0x76;
  cmdWrite(&commit,1,true);
  Serial.println("[B] Commit 0x76 sent");

  // 일부 기기는 완료 notify를 안 보냄. 2초 대기 후 성공 처리.
  uint32_t waitT = millis();
  while (millis()-waitT < 2000){
    delay(10);
  }
  return true;
}

// ------------------- Orchestration -------------------
static void wifiOnAndConnect(){
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0=millis();
  while(WiFi.status()!=WL_CONNECTED && millis()-t0<25000){ delay(150); }
  if(WiFi.status()==WL_CONNECTED) Serial.printf("[Wi-Fi] connected: %s\n", WiFi.localIP().toString().c_str());
  else                            Serial.println("[Wi-Fi] NOT connected");
}

static void wifiOff(){
  WiFi.disconnect(true, true);
  esp_wifi_stop();
  WiFi.mode(WIFI_OFF);
  delay(200);
}

void setup(){
  Serial.begin(115200);
  delay(300);

  esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT);

  Serial.println("\n====================================");
  Serial.println(" ESL 296x128 BWR — Dooray 4 Rooms (v3.9 dual-proto)");
  Serial.println(" Wi-Fi fetch → Wi-Fi OFF → BLE upload");
  Serial.println("====================================");

  // Step 1) Wi-Fi ON → fetch
  wifiOnAndConnect();
  if (WiFi.status()==WL_CONNECTED){
    ensureTime();
    for(int i=0;i<NUM_TAGS;i++){
      Serial.printf("\n[Fetch %d/%d] %s (%s)\n", i+1, NUM_TAGS, TAGS[i].label, TAGS[i].resId);
      bool ok = fetchReservationsToday(TAGS[i].resId, g_room[i]);
      Serial.printf("[Dooray] %s: %s (count=%d)\n", TAGS[i].label, ok?"OK":"FAIL", g_room[i].count);
    }
  } else {
    for(int i=0;i<NUM_TAGS;i++){ g_room[i].ok=false; g_room[i].count=0; }
  }

  // Step 2) Wi-Fi OFF
  wifiOff();

  // BLE init
  BLEDevice::init("ESL-Uploader");

  // Step 3) Render + Upload
  for(int i=0;i<NUM_TAGS;i++){
    Serial.printf("\n[Render %d/%d] %s — busy=%s\n", i+1, NUM_TAGS, TAGS[i].label, timeBusyNow(g_room[i])?"Y":"N");
    drawScreen(TAGS[i], g_room[i]);
    buildPayload();

    Serial.printf("[Upload %d/%d] %s (%s)\n", i+1, NUM_TAGS, TAGS[i].label, TAGS[i].mac);

    bool ok=false;
    for(int attempt=0; attempt<3 && !ok; ++attempt){
      if (!connectAndDiscover(TAGS[i])){
        Serial.printf("[BLE] connect/discover fail (try %d)\n", attempt+1);
        disconnectAndCleanup(); delay(500); continue;
      }

      // 먼저 A 시도 → 실패시 B
      ok = tryProtocolA();
      if (!ok){
        Serial.println("[Info] Fallback to raw 0x75");
        ok = tryProtocolB();
      }

      disconnectAndCleanup();
      if (!ok){ Serial.printf("[Retry] %s (%d/3)\n", TAGS[i].label, attempt+1); delay(700); }
    }
    Serial.printf("[Result] %s: %s\n", TAGS[i].label, ok?"성공":"실패");
    if(i<NUM_TAGS-1) delay(900);
  }

  Serial.println("\n=== All done ===");
}

void loop(){ delay(1000); }
