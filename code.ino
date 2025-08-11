// ===== deps =====
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <time.h>
#include <esp_sntp.h>
#include <algorithm>

#include <LovyanGFX.hpp>
#define LGFX_AUTODETECT
#include <LGFX_AUTODETECT.hpp>

#include <U8g2lib.h>
static lgfx::U8g2font u8g2_kor(u8g2_font_unifont_t_korean2);

// ===== user config =====
static const char* WIFI_SSID = "Smart Global";
static const char* WIFI_PASS = "";

static const char* DOORAY_TOKEN = "YOUR_TOKEN_HERE";
static const char* DOORAY_BASE  = "https://api.dooray.com";

// 회의실 4개 고정
struct Room { const char* id; const char* label; };
Room ROOMS[] = {
  {"3868297860122914233", "A동-6층 소회의실3"},
  {"3868312518617103681", "B동-6층 소회의실1"},
  {"3868312634809468080", "B동-6층 소회의실2"},
  {"3868312748489680534", "B동-6층 중회의실1"}
};
constexpr int ROOM_COUNT = sizeof(ROOMS)/sizeof(ROOMS[0]);
int curRoom = 0;

// ===== gfx =====
static LGFX lcd;

// 레이아웃
int TOP_H  = 30;    // 시계/날짜
int NAV_H  = 40;    // 회의실 라벨(축소)
int LIST_Y_POS;
int LIST_HEIGHT;
const int TIME_BOX_W = 96;  // 시계 영역 고정 폭

// 버튼
struct Btn { int x,y,w,h; const char* text; uint16_t col; };
Btn btnPrev, btnNext, btnRefresh;
Btn btnPgUp, btnPgDn;       // 목록 페이지 업/다운

// 팔레트
constexpr uint16_t C_BG     = TFT_WHITE;
constexpr uint16_t C_TEXT   = TFT_BLACK;
constexpr uint16_t C_MUTED  = TFT_DARKGREY;
constexpr uint16_t C_LINE   = TFT_LIGHTGREY;
constexpr uint16_t C_ACCENT = 0x07E0;  // free 초록
constexpr uint16_t C_ALERT  = 0xF800;  // busy 빨강
constexpr uint16_t C_BTN    = 0xD6BA;  // 버튼 배경
constexpr uint16_t C_BTN2   = 0xE6FF;  // R 버튼 배경
constexpr uint16_t C_HILITE = 0xFFE0;  // 진행중 하이라이트
constexpr uint16_t C_WARN   = 0xFFE0;  // 경고 아이콘(노랑)

// ===== 공용 유틸/터치 =====
static inline bool hit(const Btn& b, int32_t x, int32_t y){
  return (x>=b.x && x<=b.x+b.w && y>=b.y && y<=b.y+b.h);
}

// ===== 시간(KST) =====
static inline void setTZ_KST(){ setenv("TZ","KST-9",1); tzset(); }
bool timeReady(){ return time(nullptr) > 1700000000; }
static bool sntpStarted=false;

static void startSNTPOnce(){
  if (sntpStarted) return;
  setTZ_KST();
  configTzTime("KST-9","kr.pool.ntp.org","time.google.com","time.cloudflare.com");
  sntpStarted=true;
}

// UTC용 mktime(보드 호환)
time_t mktime_utc(struct tm* tmv){
  char* oldtz = getenv("TZ");
  setenv("TZ","UTC0",1); tzset();
  time_t t = mktime(tmv);
  if (oldtz) setenv("TZ",oldtz,1); else unsetenv("TZ");
  tzset();
  return t;
}

// HTTP Date 폴백
bool httpDateFallback(){
  WiFiClientSecure cli; cli.setInsecure();
  HTTPClient http; http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.begin(cli, String(DOORAY_BASE) + "/");
  const char* keys[]={"Date"}; http.collectHeaders(keys,1);
  int code=http.GET(); String date=(code>0)?http.header("Date"):""; http.end();
  if (date.length()<10) return false;
  int d,Y,h,m,s; char Mon[4];
  if (sscanf(date.c_str(),"%*3s, %d %3s %d %d:%d:%d GMT",&d,Mon,&Y,&h,&m,&s)!=6) return false;
  const char* tbl="JanFebMarAprMayJunJulAugSepOctNovDec";
  const char* p=strstr(tbl,Mon); if(!p) return false; int mon=(p-tbl)/3+1;
  struct tm tmv={}; tmv.tm_year=Y-1900; tmv.tm_mon=mon-1; tmv.tm_mday=d;
  tmv.tm_hour=h; tmv.tm_min=m; tmv.tm_sec=s;
  time_t gmt=mktime_utc(&tmv); if (gmt<=0) return false;
  struct timeval tv={gmt,0}; settimeofday(&tv,nullptr);
  setTZ_KST();  // KST 유지
  return true;
}

void ensureTime(){
  if (timeReady()) { setTZ_KST(); return; }
  startSNTPOnce();
  struct tm ti;
  for(int i=0;i<40;i++){ if(getLocalTime(&ti,500)){ setTZ_KST(); return; } }
  if (httpDateFallback()) { setTZ_KST(); return; }
  for(int i=0;i<20;i++){ if(getLocalTime(&ti,500)){ setTZ_KST(); return; } }
}

// ===== Dooray API =====
String authHeader(){ return String("dooray-api ")+DOORAY_TOKEN; }
void httpSetup(HTTPClient& http, WiFiClientSecure& cli, const String& url){
  cli.setInsecure();
  http.setConnectTimeout(15000);
  http.setTimeout(20000);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.addHeader("Authorization",authHeader());
  http.addHeader("Accept","application/json");
  http.addHeader("Accept-Encoding","identity");
  http.addHeader("Content-Type","application/json");
  http.addHeader("User-Agent","ESP32-DOORAY/1.0");
  http.begin(cli,url);
}

struct Resv{
  String subject,startISO,endISO,who;
  time_t startEpoch=0,endEpoch=0;
};
Resv resv[32]; int resvCount=0;

// ISO(+TZ/Z/소수초) → epoch(UTC)
bool isoToEpochUTC(String iso, time_t& out){
  iso.trim(); if (iso.isEmpty()) return false;
  int dot=iso.indexOf('.'); if(dot>=0){
    int tzpos=iso.indexOf('Z',dot);
    if (tzpos<0){ int p1=iso.indexOf('+',dot), p2=iso.indexOf('-',dot); tzpos=(p1>=0)?p1:p2; }
    if (tzpos>dot) iso.remove(dot, tzpos-dot); else iso.remove(dot);
  }
  bool isUTCZ=false; if (iso.endsWith("Z")){ isUTCZ=true; iso.remove(iso.length()-1); }
  int ofsSignPos=-1; for(int i=iso.length()-6;i>=0 && i<(int)iso.length();--i){ char c=iso[i]; if(c=='+'||c=='-'){ ofsSignPos=i; break; } }
  int Y=iso.substring(0,4).toInt();
  int M=iso.substring(5,7).toInt();
  int D=iso.substring(8,10).toInt();
  int h=iso.substring(11,13).toInt();
  int m=iso.substring(14,16).toInt();
  int s=(iso.length()>=19)? iso.substring(17,19).toInt() : 0;
  int off=0;
  if(!isUTCZ && ofsSignPos>0){
    char sign=iso.charAt(ofsSignPos);
    int tzH=iso.substring(ofsSignPos+1,ofsSignPos+3).toInt();
    int tzM=iso.substring(ofsSignPos+4,ofsSignPos+6).toInt();
    off = tzH*3600 + tzM*60; if (sign=='-') off = -off;
  }
  struct tm tmv={}; tmv.tm_year=Y-1900; tmv.tm_mon=M-1; tmv.tm_mday=D;
  tmv.tm_hour=h; tmv.tm_min=m; tmv.tm_sec=s;
  time_t baseUTC=mktime_utc(&tmv);
  out = isUTCZ ? baseUTC : (baseUTC - off);
  return out>0;
}

// KST 오늘 00~24
void kstDayRangeISO(String& tMin,String& tMax){
  time_t now=time(nullptr); struct tm lt; localtime_r(&now,&lt);
  lt.tm_hour=0; lt.tm_min=0; lt.tm_sec=0; time_t start=mktime(&lt);
  time_t end=start+24*3600;
  char b1[40],b2[40]; strftime(b1,sizeof(b1),"%Y-%m-%dT%H:%M:%S+09:00",&lt);
  struct tm lt2; localtime_r(&end,&lt2); strftime(b2,sizeof(b2),"%Y-%m-%dT%H:%M:%S+09:00",&lt2);
  tMin=b1; tMax=b2;
}

String hhmm(const String& iso){
  int tpos=iso.indexOf('T'); if(tpos<0) return "--:--";
  return iso.substring(tpos+1,tpos+3) + ":" + iso.substring(tpos+4,tpos+6);
}

bool fetchResvToday(const char* roomId, String& err){
  resvCount=0; ensureTime(); if(!timeReady()){ err="시간 미동기화"; return false; }
  String tMin,tMax; kstDayRangeISO(tMin,tMax);
  String url=String(DOORAY_BASE)+"/reservation/v1/resource-reservations?resourceIds="+roomId+"&timeMin="+tMin+"&timeMax="+tMax;
  WiFiClientSecure cli; HTTPClient http; httpSetup(http,cli,url);
  int code=http.GET(); String body=(code>0)?http.getString():""; http.end();
  if(code!=200 || body.length()==0){ err="HTTP "+String(code); return false; }

  StaticJsonDocument<768> filter; JsonObject item=filter["result"].createNestedObject();
  item["subject"]=true; item["startedAt"]=true; item["endedAt"]=true;
  item["users"]["from"]["type"]=true;
  item["users"]["from"]["member"]["name"]=true;
  item["users"]["from"]["emailUser"]["name"]=true;
  item["users"]["from"]["emailUser"]["emailAddress"]=true;

  DynamicJsonDocument doc(16384);
  DeserializationError e=deserializeJson(doc,body,DeserializationOption::Filter(filter));
  if(e){ err=String("JSON ")+e.c_str(); return false; }
  JsonArray arr=doc["result"].as<JsonArray>(); if(arr.isNull()){ err="no result"; return false; }

  for(JsonVariant v:arr){
    if(resvCount>= (int)(sizeof(resv)/sizeof(resv[0]))) break;
    Resv r;
    r.subject = (const char*)(v["subject"] | "");
    if(r.subject.length()==0) r.subject="(제목 없음)";
    r.startISO=(const char*)(v["startedAt"] | "");
    r.endISO  =(const char*)(v["EndedAt"]   | v["endedAt"] | ""); // 일부 응답 보정

    const char* ftype = v["users"]["from"]["type"] | "";
    String who;
    if (strcmp(ftype,"member")==0) who=(const char*)(v["users"]["from"]["member"]["name"] | "");
    else if (strcmp(ftype,"emailUser")==0){
      who=(const char*)(v["users"]["from"]["emailUser"]["name"] | "");
      if(who.length()==0) who=(const char*)(v["users"]["from"]["emailUser"]["emailAddress"] | "");
    }
    if(who.length()==0) who="예약자";
    r.who=who;

    isoToEpochUTC(r.startISO,r.startEpoch);
    isoToEpochUTC(r.endISO  ,r.endEpoch);
    if(r.endEpoch>r.startEpoch && r.startEpoch>0) resv[resvCount++]=r;
  }

  // 시간순 정렬(진행중을 맨 위로 올리지 않음)
  std::sort(resv,resv+resvCount,[](const Resv&a,const Resv&b){ return a.startEpoch<b.startEpoch; });
  return true;
}

// ===== UI =====
String lastClock;
const int lineH=24;   // 줄간격 유지
int listPage = 0;     // 목록 페이지 인덱스
uint32_t lastWifiDrawMs = 0;

// 공통 작은 버튼
void drawSmallButton(const Btn& b, bool filled=true){
  if (filled) lcd.fillRoundRect(b.x,b.y,b.w,b.h,6,b.col);
  lcd.drawRoundRect(b.x,b.y,b.w,b.h,6,C_LINE);
  lcd.setFont(&u8g2_kor);
  lcd.setTextDatum(MC_DATUM);
  lcd.setTextColor(TFT_BLACK, filled? b.col : C_BG);
  if (b.text && *b.text) lcd.drawString(b.text, b.x + b.w/2, b.y + b.h/2);
}

// Wi-Fi 막대 (0~4), -1은 미연결
int wifiBars(){
  if (WiFi.status() != WL_CONNECTED) return -1;
  long rssi = WiFi.RSSI(); // dBm
  if (rssi >= -55) return 4;
  if (rssi >= -65) return 3;
  if (rssi >= -75) return 2;
  if (rssi >= -85) return 1;
  return 0;
}

void drawWarnTriangle(int x, int y){
  // 작은 경고 삼각형(노랑) 12x12
  int s = 12;
  lcd.fillTriangle(x, y+s-1,  x+s/2, y,  x+s-1, y+s-1, C_WARN);
  lcd.drawTriangle(x, y+s-1,  x+s/2, y,  x+s-1, y+s-1, TFT_BLACK);
  // 느낌표
  lcd.drawFastVLine(x+s/2, y+3, 6, TFT_BLACK);
  lcd.drawPixel(x+s/2, y+s-2, TFT_BLACK);
}

// R 오른쪽에 Wi-Fi 표시(끊기면 X + 경고 삼각형)
void drawWiFiIndicator(){
  int x = (TIME_BOX_W + 8) + 22 + 6; // R 버튼 오른쪽 여백
  int y = 6;
  int w = 26;
  int h = TOP_H - 12;

  // 영역 클리어
  lcd.fillRect(x, y, w, h, C_BG);

  int bars = wifiBars();
  if (bars < 0){
    // 끊김: 회색 박스 + X + 경고 삼각형
    lcd.drawRect(x+1, y+1, w-2, h-2, C_LINE);
    lcd.drawLine(x+3, y+3, x+w-3, y+h-3, TFT_RED);
    lcd.drawLine(x+3, y+h-3, x+w-3, y+3, TFT_RED);
    drawWarnTriangle(x+w+2, y);   // 바로 오른쪽에 경고 아이콘
    return;
  }

  int bw = 3, gap = 3;
  int base = y + h - 1;
  for (int i=0;i<4;i++){
    int bh = (h * (i+1)) / 5;           // 단계별 높이
    int bx = x + 2 + i*(bw+gap);
    int by = base - bh;
    uint16_t col = (i < bars) ? TFT_BLACK : C_LINE;
    lcd.fillRect(bx, by, bw, bh, col);
  }
}

const char* KWD[7] = {"일","월","화","수","목","금","토"};

void drawTopBar(){
  lcd.fillRect(0,0,lcd.width(),TOP_H,C_BG);
  lcd.drawFastHLine(0,TOP_H-1,lcd.width(),C_LINE);

  ensureTime();
  time_t now=time(nullptr); if(now<1700000000) return;
  struct tm lt; localtime_r(&now,&lt);

  lcd.setFont(&u8g2_kor);
  lcd.setTextColor(C_TEXT,C_BG);
  lcd.setTextDatum(TL_DATUM);

  // 시계(좌측 고정 폭)
  char clockBuf[16]; strftime(clockBuf,sizeof(clockBuf),"%H:%M:%S",&lt);
  lcd.fillRect(0,0,TIME_BOX_W,TOP_H,C_BG);
  lcd.drawString(clockBuf,8,6);

  // 새로고침 'R' 버튼(시계 오른쪽)
  btnRefresh = { TIME_BOX_W + 8, 6, 22, TOP_H-12, "R", C_BTN2 };
  drawSmallButton(btnRefresh);

  // Wi-Fi 아이콘(+경고)
  drawWiFiIndicator();

  // 날짜(우측 정렬)
  char dateBuf[24]; strftime(dateBuf,sizeof(dateBuf),"%Y-%m-%d",&lt);
  String dateK = String(dateBuf) + " (" + KWD[lt.tm_wday] + ")";
  lcd.setTextDatum(TR_DATUM);
  lcd.drawString(dateK, lcd.width()-8, 6);
}

bool currentRoomBusy(){
  time_t now=time(nullptr);
  for(int i=0;i<resvCount;i++) if(now>=resv[i].startEpoch && now<resv[i].endEpoch) return true;
  return false;
}

void drawNavBar(){
  lcd.fillRect(0,TOP_H,lcd.width(),NAV_H,C_BG);
  lcd.drawFastHLine(0,TOP_H+NAV_H-1,lcd.width(),C_LINE);

  // 좌/우 버튼(슬림)
  btnPrev = { 8, TOP_H+6, 34, NAV_H-12, "<", C_BTN };
  btnNext = { lcd.width()-42, TOP_H+6, 34, NAV_H-12, ">", C_BTN };
  drawSmallButton(btnPrev);
  drawSmallButton(btnNext);

  // 중앙 회의실 칩(작게, 선명)
  int chipX = btnPrev.x + btnPrev.w + 8;
  int chipW = lcd.width() - (btnPrev.w + btnNext.w + 32);
  int chipY = TOP_H + 6;
  int chipH = NAV_H - 12;
  bool busy = currentRoomBusy();
  lcd.fillRoundRect(chipX,chipY,chipW,chipH,12, busy? C_ALERT : C_ACCENT);
  lcd.setTextColor(TFT_WHITE);
  lcd.setFont(&u8g2_kor);
  lcd.setTextDatum(MC_DATUM);
  lcd.setTextSize(1);
  lcd.drawString(ROOMS[curRoom].label, chipX + chipW/2, chipY + chipH/2);
  lcd.setTextSize(1);
}

void drawList(){
  int y0 = TOP_H + NAV_H + 8;
  int hArea = lcd.height() - y0 - 10; // 하단 거의 끝까지 사용
  if (hArea < 0) hArea = 0;

  lcd.fillRect(0,y0-2,lcd.width(),hArea+4,C_BG);

  lcd.setFont(&u8g2_kor);
  lcd.setTextDatum(TL_DATUM);
  lcd.setTextWrap(false);

  if(resvCount==0){
    lcd.setTextColor(C_MUTED,C_BG);
    lcd.drawString("예약 없음", 10, y0);
    // 페이지 버튼 숨김
    btnPgUp = {0,0,0,0,"",0}; btnPgDn = {0,0,0,0,"",0};
    return;
  }

  int linesPerPage = hArea / lineH;
  if (linesPerPage < 1) linesPerPage = 1;

  int totalPages = (resvCount + linesPerPage - 1) / linesPerPage;
  if (listPage >= totalPages) listPage = totalPages - 1;
  if (listPage < 0) listPage = 0;

  int startIdx = listPage * linesPerPage;
  int endIdx   = std::min(resvCount, startIdx + linesPerPage);

  int y = y0;
  time_t now=time(nullptr);

  for(int i=startIdx; i<endIdx; ++i){
    bool on=(now>=resv[i].startEpoch && now<resv[i].endEpoch);
    uint16_t bg= on ? C_HILITE : (((i-startIdx)%2==0)? C_BG : 0xFFF6);

    lcd.fillRect(0,y-2,lcd.width(),lineH+4,bg);
    lcd.setTextColor(C_TEXT,bg);

    String who = resv[i].who.length()? (" / "+resv[i].who) : "";
    String line = hhmm(resv[i].startISO) + "-" + hhmm(resv[i].endISO) + "  " + resv[i].subject + who;
    if(line.length()>60) line=line.substring(0,60);
    lcd.drawString(line,10,y);

    // 상태 점
    lcd.fillCircle(4, y + lineH/2, 3, on? C_ALERT : C_LINE);

    y += lineH;
  }

  // 페이지 버튼(필요할 때만 표시)
  if (totalPages > 1){
    int bx = lcd.width()-30;
    btnPgUp = { bx, y0, 22, 22, "▲", C_BTN };
    btnPgDn = { bx, y0 + hArea - 22, 22, 22, "▼", C_BTN };
    drawSmallButton(btnPgUp);
    drawSmallButton(btnPgDn);
  } else {
    btnPgUp = {0,0,0,0,"",0};
    btnPgDn = {0,0,0,0,"",0};
  }
}

// 시계/와이파이 주기 갱신
void updateClockIfChanged(){
  if(!timeReady()) return;
  time_t now=time(nullptr); struct tm lt; localtime_r(&now,&lt);
  char buf[16]; strftime(buf,sizeof(buf),"%H:%M:%S",&lt);
  static String last=""; String cur(buf);
  if(cur!=last){
    last=cur;
    lcd.fillRect(0,0,TIME_BOX_W,TOP_H,C_BG);
    lcd.setFont(&u8g2_kor);
    lcd.setTextColor(C_TEXT,C_BG);
    lcd.setTextDatum(TL_DATUM);
    lcd.drawString(cur,8,6);
  }

  if (millis() - lastWifiDrawMs > 1500) { // 1.5초마다 상태 갱신
    drawWiFiIndicator();
    lastWifiDrawMs = millis();
  }
}

// ===== Wi-Fi 재연결 루프(10초 간격) =====
static uint32_t lastWifiAttemptMs = 0;
static bool wifiEverConnected = false;
void wifiReconnectLoop(){
  if (WiFi.status() == WL_CONNECTED) { wifiEverConnected = true; return; }
  if (millis() - lastWifiAttemptMs < 10000) return; // 10초 간격
  lastWifiAttemptMs = millis();
  if (wifiEverConnected) {
    WiFi.reconnect();   // 이전에 붙은 적 있으면 우선 재연결
  } else {
    WiFi.begin(WIFI_SSID, WIFI_PASS); // 처음부터 실패했다면 다시 시도
  }
}

// 갱신
uint32_t lastFetchMs=0;
const uint32_t REFRESH_MS=180000; // 3분

void drawTopAll(){ drawTopBar(); drawNavBar(); }

void refetchAndRedraw(){
  String err;
  (void)fetchResvToday(ROOMS[curRoom].id, err); // 실패해도 이전 화면 유지
  drawTopAll();
  drawList();
  lastFetchMs=millis();
}

// ★ 풀 리프레시(롱프레스용): 시간 재동기 + 캐시 초기화
void fullRefresh(){
  listPage = 0;
  resvCount = 0;
  lastClock = "";
  sntpStarted = false;   // SNTP 재시작 허용
  setTZ_KST();
  ensureTime();
  refetchAndRedraw();
}

// ===== setup/loop =====
void setup(){
  Serial.begin(115200);
  lcd.init(); lcd.setRotation(1); lcd.setBrightness(200); lcd.fillScreen(C_BG);

  LIST_Y_POS = TOP_H + NAV_H + 8;
  LIST_HEIGHT = lcd.height() - LIST_Y_POS - 10;

  // Wi-Fi
  lcd.setFont(&u8g2_kor); lcd.setTextColor(C_TEXT,C_BG); lcd.setTextDatum(TL_DATUM);
  lcd.drawString("Wi-Fi 연결 중...",10,8);

  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID,WIFI_PASS);
  uint32_t t0=millis(); while(WiFi.status()!=WL_CONNECTED && millis()-t0<20000){ delay(200); }
  lcd.fillRect(0,8,lcd.width(),20,C_BG);
  if(WiFi.status()!=WL_CONNECTED){ lcd.setTextColor(TFT_RED,C_BG); lcd.drawString("Wi-Fi 실패",10,8); }
  else { wifiEverConnected = true; lcd.setTextColor(C_TEXT,C_BG); lcd.drawString("IP: "+WiFi.localIP().toString(),10,8); }

  setTZ_KST();
  ensureTime();
  refetchAndRedraw();
}

void loop(){
  updateClockIfChanged();
  wifiReconnectLoop();                 // ★ 10초 간격 재연결

  if(millis()-lastFetchMs > REFRESH_MS) refetchAndRedraw();

  // ----- 터치/제스처/롱프레스 -----
  static bool touching=false, pressRefresh=false;
  static int32_t sx=0, sy=0, lx=0, ly=0;
  static uint32_t st=0;
  int32_t x=-1,y=-1;
  bool cur = lcd.getTouch(&x,&y);

  const int SWIPE_MIN = 40;
  const int LONGPRESS_MS = 800;
  const int TAP_MAX_MOVE = 15;

  if (cur){
    if (!touching){
      touching=true; sx=x; sy=y; st=millis();
      pressRefresh = hit(btnRefresh, x, y);   // 롱프레스 후보
    }
    lx=x; ly=y;
  } else if (touching){
    int dx = lx - sx, dy = ly - sy;
    int adx = dx>0?dx:-dx, ady = dy>0?dy:-dy;
    uint32_t dur = millis() - st;

    bool smallMove = (adx < TAP_MAX_MOVE && ady < TAP_MAX_MOVE);

    if (pressRefresh && hit(btnRefresh,lx,ly) && smallMove){
      // R 버튼 탭/롱프레스
      if (dur >= LONGPRESS_MS) fullRefresh();
      else { listPage=0; refetchAndRedraw(); }
    } else if ((adx>SWIPE_MIN || ady>SWIPE_MIN) && dur<800){
      // 스와이프
      if (adx > ady*1.2){
        if (dx > SWIPE_MIN){ // →
          curRoom=(curRoom+ROOM_COUNT-1)%ROOM_COUNT; listPage=0; refetchAndRedraw();
        } else if (dx < -SWIPE_MIN){ // ←
          curRoom=(curRoom+1)%ROOM_COUNT; listPage=0; refetchAndRedraw();
        }
      } else if (ady > adx*1.2){
        if (dy > SWIPE_MIN){ // ↓
          listPage++; drawList();
        } else if (dy < -SWIPE_MIN){ // ↑
          listPage = (listPage>0? listPage-1 : 0); drawList();
        }
      }
    } else {
      // 일반 탭
      if(hit(btnPrev,lx,ly)){ curRoom=(curRoom+ROOM_COUNT-1)%ROOM_COUNT; listPage=0; refetchAndRedraw(); }
      else if(hit(btnNext,lx,ly)){ curRoom=(curRoom+1)%ROOM_COUNT; listPage=0; refetchAndRedraw(); }
      else if(btnPgUp.w && hit(btnPgUp,lx,ly)){ listPage = (listPage>0? listPage-1 : 0); drawList(); }
      else if(btnPgDn.w && hit(btnPgDn,lx,ly)){ listPage++; drawList(); }
    }
    touching=false; pressRefresh=false;
  }
}
