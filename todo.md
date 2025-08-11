**2순위로 `iptime` 자동 폴백** 
아래처럼 “여러 SSID 우선순위 연결 + 10초마다 재시도(SSID 순환)”로 바꿔줘. 기존 `WIFI_SSID/WIFI_PASS`는 지우고, 이 블록들만 추가/교체하면 돼.

### 1) 상단 설정부에 “우선순위 Wi-Fi 목록” 추가

```cpp
// ---- Wi-Fi priority list ----
struct WiFiCred { const char* ssid; const char* pass; };
WiFiCred WIFI_LIST[] = {
  {"Smart Global", ""},   // 1순위
  {"iptime", ""}          // 2순위 (폴백)
};
constexpr int WIFI_COUNT = sizeof(WIFI_LIST)/sizeof(WIFI_LIST[0]);
int wifiIdx = 0;                 // 현재 시도 중인 SSID 인덱스
```

### 2) 헬퍼 함수 추가

```cpp
bool waitConnect(uint32_t ms=20000){
  uint32_t t0 = millis();
  while (WiFi.status()!=WL_CONNECTED && millis()-t0 < ms) { delay(200); }
  return WiFi.status()==WL_CONNECTED;
}

void wifiBeginIdx(int idx){
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_LIST[idx].ssid, WIFI_LIST[idx].pass);
}

void wifiConnectInitial(){
  for (int i=0; i<WIFI_COUNT; ++i){
    wifiIdx = i;
    wifiBeginIdx(wifiIdx);
    if (waitConnect()) return;
  }
}
```

### 3) `setup()`의 Wi-Fi 부분 교체

```cpp
// --- Wi-Fi ---
lcd.setFont(&u8g2_kor); lcd.setTextColor(C_TEXT,C_BG); lcd.setTextDatum(TL_DATUM);
lcd.drawString("Wi-Fi 연결 중...", 10, 8);

wifiConnectInitial();

lcd.fillRect(0,8,lcd.width(),20,C_BG);
if (WiFi.status()!=WL_CONNECTED){
  lcd.setTextColor(TFT_RED, C_BG);
  lcd.drawString("Wi-Fi 실패", 10, 8);
} else {
  wifiEverConnected = true;
  lcd.setTextColor(C_TEXT, C_BG);
  lcd.drawString("IP: " + WiFi.localIP().toString() + " (" + String(WIFI_LIST[wifiIdx].ssid) + ")", 10, 8);
}
```

### 4) 재연결 루프를 “SSID 순환” 버전으로 교체

(지금 있는 `wifiReconnectLoop()`를 아래로 바꿔치기)

```cpp
static uint32_t lastWifiAttemptMs = 0;
static bool wifiEverConnected = false;
static uint8_t retryCount = 0;
const uint8_t MAX_RETRY_PER_SSID = 3;  // SSID당 3번 재시도 후 다음 SSID로

void wifiReconnectLoop(){
  if (WiFi.status() == WL_CONNECTED) {
    wifiEverConnected = true;
    retryCount = 0;
    return;
  }
  if (millis() - lastWifiAttemptMs < 10000) return; // 10초 간격
  lastWifiAttemptMs = millis();

  if (retryCount < MAX_RETRY_PER_SSID && wifiEverConnected) {
    WiFi.reconnect();           // 현재 SSID 재시도
    retryCount++;
  } else {
    // 다음 SSID로 순환
    retryCount = 0;
    wifiIdx = (wifiIdx + 1) % WIFI_COUNT;
    WiFi.disconnect(true, true);
    delay(100);
    wifiBeginIdx(wifiIdx);
  }
}
```

이렇게 하면:

* 기본은 **Smart Global**에 붙고, 안 되면 **iptime**으로 자동 폴백.
* 연결 끊기면 10초 간격으로 현재 SSID를 3번 재시도 → 그래도 실패면 **다음 SSID로 자동 전환**.
* 상단 Wi-Fi 막대/경고 아이콘 로직은 그대로 동작.
* `setup()`의 표시엔 현재 붙은 SSID도 같이 찍어줘서 확인 쉬움.

원하면 SSID 더 추가해도 돼요(그냥 `WIFI_LIST`에 한 줄씩 추가).
