# esp32\_dooray\_pkg

ESP32 + LovyanGFX 터치 TFT로 **Dooray! 회의실 예약 현황**을 표시하는 프로젝트입니다.
한국 시간(KST) 기준으로 오늘 예약을 시간순으로 보여주고, 진행 중 일정은 하이라이트됩니다. 로딩바 없이 조용히 갱신되며, 상단에 시계/날짜/새로고침(R)/Wi-Fi 상태가 표시됩니다.

---

## 주요 기능

* **Dooray! 자원 예약 조회**: 오늘(00–24시, KST)의 예약을 API로 가져와 표시
* **시간순 정렬** + **진행 중 하이라이트**(맨 위 고정은 하지 않음)
* **상단 UI**: 시계(좌), 새로고침 **R** 버튼, Wi-Fi 막대(에러 시 경고), 날짜(우)
* **새로고침**

  * 자동: 3분 간격
  * 탭: 즉시 새로고침
  * **롱프레스(>800ms)**: **풀 리프레시**(시간 재동기 + 캐시 초기화)
* **Wi-Fi 안정성**

  * 신호 막대 0–4칸, 끊김 시 박스+X + 경고 삼각형
  * **우선순위 SSID 리스트**(예: “Smart Global” → “iptime”) **자동 폴백**
  * 끊기면 **10초 간격 재시도**, SSID당 3회 실패 시 다음 SSID로 순환
* **페이지 이동**

  * 화면에 다 못 담으면 ▲/▼ **페이지 버튼** 자동 노출
  * **스와이프 제스처**: 좌/우(방 전환), 상/하(페이지 이동)
* **한국어 폰트**: U8g2 한글 유니폰트(저용량)

---

## 폴더 구조

```
esp32_dooray_pkg/
├─ code.ino                // 메인 스케치
├─ dooray_config.h         // Dooray! 토큰/자원ID 등 사용자 설정
├─ touch_config.h          // 터치/패널 보정 설정
├─ XH32S_TFT_Setup.h       // TFT 설정(필요 시 보드별 커스텀)
└─ README.md               // 이 문서
```

---

## 하드웨어 요구사항

* ESP32 보드(예: ESP32 Dev Module)
* SPI 터치 TFT 모듈 (LovyanGFX 지원 패널)
* 터치 패널(저항식/정전식 중 보유 패널에 맞게 설정)
* 안정적인 5V/3.3V 전원

> 디스플레이/터치 핀 매핑은 `XH32S_TFT_Setup.h`, `touch_config.h`에서 조정하세요. 기본은 `LGFX_AUTODETECT` 사용.

---

## 소프트웨어/라이브러리

* Arduino IDE (또는 PlatformIO)
* **ESP32 보드 패키지** by Espressif
* 라이브러리:

  * `WiFi.h`, `WiFiClientSecure.h`, `HTTPClient.h`
  * **ArduinoJson**
  * **LovyanGFX**
  * **U8g2** (u8g2\_font\_unifont\_t\_korean2 사용)

---

## 설정(필수)

### 1) Dooray! 토큰/자원 설정

`dooray_config.h`에 개인 API 토큰과 자원(회의실) ID, 라벨을 입력하세요.

```cpp
// dooray_config.h (예시)
static const char* DOORAY_TOKEN = "YOUR_TOKEN_HERE";
static const char* DOORAY_BASE  = "https://api.dooray.com";

// 방 목록 (ID 고정, 라벨 한글)
struct Room { const char* id; const char* label; };
Room ROOMS[] = {
  {"3868297860122914233", "A동-6층 소회의실3"},
  {"3868312518617103681", "B동-6층 소회의실1"},
  {"3868312634809468080", "B동-6층 소회의실2"},
  {"3868312748489680534", "B동-6층 중회의실1"},
};
```

> 운영 배포 시 `WiFiClientSecure`의 `setInsecure()` 대신 **루트 CA 고정**을 권장합니다.

### 2) Wi-Fi 우선순위 목록

`code.ino`에서 SSID 우선순위를 지정합니다. (1순위 실패 시 2순위로 자동 폴백)

```cpp
struct WiFiCred { const char* ssid; const char* pass; };
WiFiCred WIFI_LIST[] = {
  {"Smart Global", ""},  // 1순위
  {"iptime", ""}         // 2순위(폴백)
};
```

### 3) 패널/터치 설정

패널에 따라 `XH32S_TFT_Setup.h`, `touch_config.h`를 조정하세요.

---

## 빌드 & 업로드

1. Arduino IDE에서 **보드**: *ESP32 Dev Module* (또는 보유 보드) 선택
2. 시리얼 속도: 115200
3. 필요한 라이브러리 설치
4. `dooray_config.h`에 토큰/자원 설정 → `Upload`
5. 부팅 후 화면 좌상단에 IP가 뜨면 정상 연결

---

## 사용법

* **상단 바**

  * **시계(좌)**: KST 기준 `HH:MM:SS`
  * **R 버튼**: 탭=즉시 새로고침 / **롱프레스(>800ms)=풀 리프레시**
  * **Wi-Fi 막대**: 0–4칸, 끊김 시 박스+X + 노란 경고 삼각형
  * **날짜(우)**: `YYYY-MM-DD (요일)`
* **중앙 네비**

  * `<` / `>` : 방 전환
  * 가운데 칩: 방 라벨(현재 진행 중이면 배경 빨강, 아니면 초록)
* **리스트**

  * **시간순 정렬**
  * **진행 중** 일정은 노란 줄 배경 하이라이트
  * 화면에 다 안 보이면 우측에 **▲/▼ 페이지 버튼**이 나타남
* **제스처**

  * 좌/우 스와이프: 방 전환
  * 상/하 스와이프: 페이지 업/다운

---

## 동작 개요

* **시간 동기화**: SNTP(`kr.pool.ntp.org`, `time.google.com`, `time.cloudflare.com`) → 실패 시 **Dooray 서버 HTTP Date** 폴백
* **표시 시간대**: **KST 고정**
* **데이터 범위**: 오늘 00:00–24:00 (KST)
* **새로고침 주기**: 3분 (수동 갱신/롱프레스 가능)
* **네트워크 안정성**

  * 끊기면 **10초 간격 재시도**
  * SSID당 3회 실패 시 **다음 SSID로 자동 전환**
  * 재연결 성공 시 지표/아이콘 자동 갱신

---

## 커스터마이즈 포인트

* **새로고침 주기**: `REFRESH_MS` (기본 180000ms)
* **롱프레스 임계값**: `LONGPRESS_MS` (기본 800ms)
* **스와이프 임계값**: `SWIPE_MIN` (기본 40px)
* **줄 간격**: `lineH` (기본 24)
* **UI 여백/크기**: `TOP_H`, `NAV_H`, 버튼 크기/위치 상수

---

## 트러블슈팅

* **시간이 이상함**

  * 네트워크가 불안정하면 SNTP가 늦을 수 있습니다. **R 롱프레스**로 **풀 리프레시(시간 재동기)** 후 확인.
* **아이콘/글자가 겹침**

  * 패널 해상도/회전에 따라 여백이 다를 수 있습니다. `TOP_H`, `NAV_H`, `TIME_BOX_W`를 조정.
* **터치가 빗나감**

  * `touch_config.h`의 보정 파라미터 조정.
* **Dooray 에러(401/403)**

  * API 토큰 또는 권한 확인. 운영 시 **루트 CA 핀닝** 권장.
* **Wi-Fi가 자주 끊김**

  * SSID 우선순위를 조정하거나 `MAX_RETRY_PER_SSID`, 재시도 간격(10초)을 늘려보세요.

---

## 개선 아이디어(로드맵)

* **“곧 시작(≤10분)”** 일정 노란 점 깜빡임
* **다음 비는 시간** 한 줄 안내(칩 아래)
* **오프라인 캐시**(이전 결과 유지 표시)
* **CA 핀닝**(운영 보안 강화)
* 제스처 민감도 UI에서 실시간 조절

---

## 크레딧

* Display: **LovyanGFX**
* Font: **U8g2**
* JSON: **ArduinoJson**
* Thanks to Dooray! API

---

필요한 스크린샷/GIF, 보정값 예시가 있으면 README에 추가해 둘게요. 추가 요청 환영!
