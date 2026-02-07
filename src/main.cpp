#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#include "secrets.h"

// ==== 設定 ====
static const uint32_t BTC_UPDATE_MS = 60 * 1000;
static const uint32_t RSS_UPDATE_MS = 5 * 60 * 1000;

static const uint32_t UI_TICK_MS    = 33;     // UI更新周期（約30fps）
static const int      SCROLL_PX_PER_TICK = 2; // ティッカー速度

static const uint32_t WIFI_TIMEOUT_MS = 15000;
static const uint32_t NTP_TIMEOUT_MS  = 8000;

// テキストバッファサイズ（必要なら増やしてOK）
static const size_t RSS_BUF_SZ    = 512;
static const size_t TICKER_BUF_SZ = 768;

// ==== 共有状態（タスク間） ====
static SemaphoreHandle_t gMutex;

static volatile int  gWiFiStatus = WL_DISCONNECTED;
static volatile bool gTimeValid  = false;
static double        gBtcJpy     = 0.0;

static char gRssTitles[RSS_BUF_SZ]   = "(pending)";
static char gPhase[32]               = "WiFi connecting"; // 起動フェーズ表示

// dirtyフラグ（UI側が差分描画するため）
static volatile bool gDirtyStatus = true;  // WiFi/Time/BTC/RSS状態
static volatile bool gDirtyTicker = true;  // ティッカー文章

enum InitState { INIT_WIFI, INIT_NTP, INIT_FETCH, RUNNING, OFFLINE };
static volatile InitState gState = INIT_WIFI;

// ==== UI（スプライトでティッカーを滑らかに） ====
static M5Canvas tickerCanvas(&M5.Display);
static bool tickerSpriteOK = false;
static int  tickerX = 0;

// UIキャッシュ（差分描画）
struct UiCache {
  char phase[32]  = "";
  char clock[16]  = "";
  char wifi[8]    = "";
  char timeMode[8]= "";
  char btc[16]    = "";
  char rss[16]    = "";
  char jstline[32]= "";
};
static UiCache uiBoot, uiRun;

static bool isTimeValidLocal() {
  time_t now = time(nullptr);
  return (now > 1600000000); // だいたい2020年以降なら同期済み扱い
}

static void updateTimeValidFlag() {
  bool v = isTimeValidLocal();
  if (v != gTimeValid) {
    gTimeValid = v;
    gDirtyStatus = true;
  }
}

static String clockText() {
  if (isTimeValidLocal()) {
    struct tm tminfo;
    if (getLocalTime(&tminfo, 0)) {
      char buf[16];
      strftime(buf, sizeof(buf), "%H:%M:%S", &tminfo);
      return String(buf);
    }
  }
  // 未同期: uptimeを時計っぽく
  uint32_t s = millis() / 1000;
  uint32_t hh = (s / 3600) % 100;
  uint32_t mm = (s / 60) % 60;
  uint32_t ss = s % 60;
  char buf[16];
  snprintf(buf, sizeof(buf), "%02lu:%02lu:%02lu",
           (unsigned long)hh, (unsigned long)mm, (unsigned long)ss);
  return String(buf);
}

static String dateTimeJST() {
  if (!isTimeValidLocal()) return "----/--/-- --:--:--";
  struct tm tminfo;
  if (!getLocalTime(&tminfo, 0)) return "----/--/-- --:--:--";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M:%S", &tminfo);
  return String(buf);
}

static void setPhase(const char* p) {
  xSemaphoreTake(gMutex, portMAX_DELAY);
  snprintf(gPhase, sizeof(gPhase), "%s", p ? p : "");
  xSemaphoreGive(gMutex);
  gDirtyStatus = true;
}

static void startWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
}

static void startNtpJST() {
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org", "time.google.com");
}

static bool httpsGetString(const char* url, String& out) {
  WiFiClientSecure client;
  client.setInsecure(); // まず動かす版（後でCA検証に差し替え可）

  HTTPClient http;
  http.setTimeout(3500);

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  out = http.getString();
  http.end();
  return true;
}

static bool fetchBtc(double& outBtc) {
  String body;
  if (!httpsGetString(BTC_URL, body)) return false;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) return false;

  double v = doc["bitcoin"]["jpy"] | 0.0;
  if (v <= 0.0) return false;
  outBtc = v;
  return true;
}

// RSSから <item><title> を拾う（ネット側でだけString使用）
static bool fetchRssTitlesToBuf(char* dst, size_t dstsz, int maxItems = 5) {
  if (!dst || dstsz == 0) return false;

  String xml;
  if (!httpsGetString(RSS_URL, xml)) return false;

  String joined;
  joined.reserve(384);

  int count = 0;
  int pos = 0;

  while (count < maxItems) {
    int itemS = xml.indexOf("<item", pos);
    if (itemS < 0) break;
    int itemE = xml.indexOf("</item>", itemS);
    if (itemE < 0) break;

    String item = xml.substring(itemS, itemE);

    int tS = item.indexOf("<title>");
    int tE = item.indexOf("</title>");
    if (tS >= 0 && tE > tS) {
      String title = item.substring(tS + 7, tE);
      title.replace("<![CDATA[", "");
      title.replace("]]>", "");
      title.replace("&amp;", "&");
      title.replace("&lt;", "<");
      title.replace("&gt;", ">");
      title.replace("&quot;", "\"");
      title.replace("&#39;", "'");

      if (joined.length()) joined += "  |  ";
      joined += title;
      count++;
    }
    pos = itemE + 7;
  }

  if (count <= 0) return false;

  // バッファへコピー（長すぎたら切る）
  snprintf(dst, dstsz, "%s", joined.c_str());
  return true;
}

// ==== 差分描画ヘルパ ====
static bool drawTextIfChanged(int x, int y, int w, int h,
                              uint16_t bg, uint16_t fg, int size,
                              const char* text, char* cache, size_t cacheN,
                              bool force = false)
{
  if (!force && text && cache && strcmp(text, cache) == 0) return false;

  M5.Display.fillRect(x, y, w, h, bg);
  M5.Display.setTextColor(fg, bg);
  M5.Display.setTextSize(size);
  M5.Display.setCursor(x, y);
  M5.Display.print(text ? text : "");
  snprintf(cache, cacheN, "%s", text ? text : "");
  return true;
}

// ==== UI：静的部分（モード切替時だけ）====
static void drawBootStatic() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);
  M5.Display.print("BOOT");
  M5.Display.drawFastHLine(0, 26, M5.Display.width(), WHITE);

  // ラベル
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 90);  M5.Display.print("WiFi:");
  M5.Display.setCursor(0, 110); M5.Display.print("Time:");
  M5.Display.setCursor(0, 130); M5.Display.print("BTC :");
  M5.Display.setCursor(0, 150); M5.Display.print("RSS :");

  // キャッシュを初期化（強制再描画を誘発）
  memset(&uiBoot, 0, sizeof(uiBoot));
  strcpy(uiBoot.phase, "!");
  strcpy(uiBoot.clock, "!");
}

static void drawRunStatic() {
  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);

  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 0);  M5.Display.print("WiFi:");
  M5.Display.setCursor(0, 20); M5.Display.print("JST :");
  M5.Display.setCursor(0, 40); M5.Display.print("BTC :");
  M5.Display.drawFastHLine(0, 70, M5.Display.width(), WHITE);

  memset(&uiRun, 0, sizeof(uiRun));
  strcpy(uiRun.wifi, "!");
  strcpy(uiRun.jstline, "!");
  strcpy(uiRun.btc, "!");
}

// ==== UI：動的部分（差分のみ）====
static void drawBootDynamic() {
  char phase[32];
  char wifi[8];
  char tmode[8];
  char btc[16];
  char rss[16];

  // 共有データ取得
  xSemaphoreTake(gMutex, portMAX_DELAY);
  snprintf(phase, sizeof(phase), "%s", gPhase);
  xSemaphoreGive(gMutex);

  snprintf(wifi, sizeof(wifi), "%s", (WiFi.status() == WL_CONNECTED) ? "OK" : "NG");
  snprintf(tmode, sizeof(tmode), "%s", gTimeValid ? "JST" : "Uptime");
  snprintf(btc, sizeof(btc), "%s", (gBtcJpy > 0.0) ? "Ready" : "Pending");
  snprintf(rss, sizeof(rss), "%s", (strlen(gRssTitles) && strcmp(gRssTitles, "(pending)") != 0) ? "Ready" : "Pending");

  // phase（上段）
  drawTextIfChanged(70, 0, 250, 22, BLACK, WHITE, 2, phase, uiBoot.phase, sizeof(uiBoot.phase));

  // clock（大きめ）
  String c = clockText();
  drawTextIfChanged(0, 30, M5.Display.width(), 55, BLACK, WHITE, 4, c.c_str(), uiBoot.clock, sizeof(uiBoot.clock));

  // status lines
  drawTextIfChanged(70, 90,  80, 18, BLACK, WHITE, 2, wifi,  uiBoot.wifi,    sizeof(uiBoot.wifi));
  drawTextIfChanged(70, 110, 80, 18, BLACK, WHITE, 2, tmode, uiBoot.timeMode,sizeof(uiBoot.timeMode));
  drawTextIfChanged(70, 130, 120,18, BLACK, WHITE, 2, btc,   uiBoot.btc,     sizeof(uiBoot.btc));
  drawTextIfChanged(70, 150, 120,18, BLACK, WHITE, 2, rss,   uiBoot.rss,     sizeof(uiBoot.rss));
}

static void buildTickerText(char* out, size_t outsz) {
  // ticker文字列をUI側で合成（ここはUIタスクのみ）
  char rss[RSS_BUF_SZ];
  double btc;

  xSemaphoreTake(gMutex, portMAX_DELAY);
  snprintf(rss, sizeof(rss), "%s", gRssTitles);
  btc = gBtcJpy;
  xSemaphoreGive(gMutex);

  String dt = dateTimeJST();

  // 可能ならここもString使わずにsnprintfだけでOK
  snprintf(out, outsz, "JST %s   BTC/JPY %lu   NEWS %s",
           dt.c_str(),
           (unsigned long)btc,
           (strlen(rss) ? rss : "(no news)"));
}

static void drawRunDynamic() {
  // WiFi
  const char* wifi = (WiFi.status() == WL_CONNECTED) ? "OK" : "NG";
  drawTextIfChanged(70, 0, 250, 18, BLACK, WHITE, 2, wifi, uiRun.wifi, sizeof(uiRun.wifi));

  // JST line（毎秒変化。変化時のみ更新）
  String dt = dateTimeJST();
  drawTextIfChanged(70, 20, 250, 18, BLACK, WHITE, 2, dt.c_str(), uiRun.jstline, sizeof(uiRun.jstline));

  // BTC
  char btc[24];
  snprintf(btc, sizeof(btc), "%.0f JPY", gBtcJpy);
  drawTextIfChanged(70, 40, 250, 18, BLACK, WHITE, 2, btc, uiRun.btc, sizeof(uiRun.btc));
}

static void drawTickerFrame(const char* tickerText) {
  const int y = 86;
  const int h = 40;

  if (tickerSpriteOK) {
    tickerCanvas.fillScreen(BLACK);
    tickerCanvas.setTextColor(WHITE, BLACK);
    tickerCanvas.setTextSize(2);
    tickerCanvas.setCursor(tickerX, 0);
    tickerCanvas.print(tickerText);
    tickerCanvas.pushSprite(0, y);
  } else {
    // フォールバック（フリックは出やすい）
    M5.Display.fillRect(0, y, M5.Display.width(), h, BLACK);
    M5.Display.setTextColor(WHITE, BLACK);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(tickerX, y);
    M5.Display.print(tickerText);
  }

  tickerX -= SCROLL_PX_PER_TICK;

  int approxW = (int)strlen(tickerText) * 12;
  if (tickerX < -approxW) tickerX = M5.Display.width();
}

// ==== タスク：ネットワーク（取得/状態管理）====
static void NetTask(void* arg) {
  (void)arg;

  uint32_t wifiStart = 0;
  uint32_t ntpStart  = 0;
  uint32_t lastBtc   = 0;
  uint32_t lastRss   = 0;

  gState = INIT_WIFI;
  setPhase("WiFi connecting");
  startWifi();
  wifiStart = millis();

  for (;;) {
    gWiFiStatus = WiFi.status();
    updateTimeValidFlag();

    uint32_t now = millis();

    switch (gState) {
      case INIT_WIFI:
        if (WiFi.status() == WL_CONNECTED) {
          setPhase("Time syncing");
          startNtpJST();
          ntpStart = now;
          gState = INIT_NTP;
          gDirtyStatus = true;
        } else if (now - wifiStart > WIFI_TIMEOUT_MS) {
          setPhase("Offline mode");
          gState = OFFLINE;
          gDirtyStatus = true;
        }
        break;

      case INIT_NTP:
        if (isTimeValidLocal()) {
          setPhase("Fetching feeds");
          gState = INIT_FETCH;
          gDirtyStatus = true;
        } else if (now - ntpStart > NTP_TIMEOUT_MS) {
          setPhase("Fetching feeds");
          gState = INIT_FETCH;
          gDirtyStatus = true;
        }
        break;

      case INIT_FETCH: {
        if (WiFi.status() == WL_CONNECTED) {
          double v;
          if (fetchBtc(v)) {
            xSemaphoreTake(gMutex, portMAX_DELAY);
            gBtcJpy = v;
            xSemaphoreGive(gMutex);
            gDirtyStatus = true;
            gDirtyTicker = true;
          }

          char buf[RSS_BUF_SZ];
          if (fetchRssTitlesToBuf(buf, sizeof(buf))) {
            xSemaphoreTake(gMutex, portMAX_DELAY);
            snprintf(gRssTitles, sizeof(gRssTitles), "%s", buf);
            xSemaphoreGive(gMutex);
            gDirtyStatus = true;
            gDirtyTicker = true;
          } else {
            xSemaphoreTake(gMutex, portMAX_DELAY);
            snprintf(gRssTitles, sizeof(gRssTitles), "%s", "(RSS fetch failed)");
            xSemaphoreGive(gMutex);
            gDirtyStatus = true;
            gDirtyTicker = true;
          }

          lastBtc = now;
          lastRss = now;
          setPhase("Running");
          gState = RUNNING;
        } else {
          xSemaphoreTake(gMutex, portMAX_DELAY);
          snprintf(gRssTitles, sizeof(gRssTitles), "%s", "(No WiFi)");
          xSemaphoreGive(gMutex);
          gDirtyStatus = true;
          gDirtyTicker = true;

          setPhase("Offline mode");
          gState = OFFLINE;
        }
      } break;

      case RUNNING:
        if (WiFi.status() != WL_CONNECTED) {
          setPhase("Offline mode");
          xSemaphoreTake(gMutex, portMAX_DELAY);
          snprintf(gRssTitles, sizeof(gRssTitles), "%s", "(WiFi lost)");
          xSemaphoreGive(gMutex);
          gDirtyStatus = true;
          gDirtyTicker = true;
          gState = OFFLINE;
          wifiStart = now;
          break;
        }

        if (now - lastBtc >= BTC_UPDATE_MS) {
          double v;
          if (fetchBtc(v)) {
            xSemaphoreTake(gMutex, portMAX_DELAY);
            gBtcJpy = v;
            xSemaphoreGive(gMutex);
            gDirtyStatus = true;
            gDirtyTicker = true;
          }
          lastBtc = now;
        }

        if (now - lastRss >= RSS_UPDATE_MS) {
          char buf[RSS_BUF_SZ];
          if (fetchRssTitlesToBuf(buf, sizeof(buf))) {
            xSemaphoreTake(gMutex, portMAX_DELAY);
            snprintf(gRssTitles, sizeof(gRssTitles), "%s", buf);
            xSemaphoreGive(gMutex);
            gDirtyStatus = true;
            gDirtyTicker = true;
          }
          lastRss = now;
        }
        break;

      case OFFLINE:
        if (WiFi.status() != WL_CONNECTED) {
          // 10秒ごとに再接続
          if (now - wifiStart > 10000) {
            setPhase("WiFi reconnect");
            startWifi();
            wifiStart = now;
            gDirtyStatus = true;
          }
        } else {
          // 復帰したらNTP→FETCHへ
          if (!isTimeValidLocal()) {
            setPhase("Time syncing");
            startNtpJST();
            ntpStart = now;
            gState = INIT_NTP;
          } else {
            setPhase("Fetching feeds");
            gState = INIT_FETCH;
          }
          gDirtyStatus = true;
        }
        break;
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

// ==== タスク：UI（描画のみ）====
static void UiTask(void* arg) {
  (void)arg;

  // 初回はBoot静的を描く
  InitState lastMode = INIT_WIFI;
  drawBootStatic();

  // ティッカー文字列
  static char ticker[TICKER_BUF_SZ];
  buildTickerText(ticker, sizeof(ticker));
  tickerX = M5.Display.width();

  uint32_t lastUi = millis();
  uint32_t lastSecond = 0;

  for (;;) {
    M5.update();
    uint32_t now = millis();

    // 1秒ごとに「時間系」だけ更新（差分描画が効く）
    uint32_t sec = now / 1000;
    if (sec != lastSecond) {
      lastSecond = sec;
      // 時刻が進んだらティッカーも更新したい人向け（必要ならON）
      gDirtyTicker = true;
    }

    if (now - lastUi >= UI_TICK_MS) {
      InitState mode = gState;

      // モード切替時は静的部分を描き直し（ここだけ全消去OK）
      if (mode != lastMode) {
        if (mode == RUNNING) drawRunStatic();
        else drawBootStatic();
        lastMode = mode;
      }

      if (mode == RUNNING) {
        // 変化したところだけ
        if (gDirtyStatus) {
          gDirtyStatus = false;
          drawRunDynamic();
        } else {
          // JSTは秒で変わるので、ここは差分で毎tick呼んでもOK
          drawRunDynamic();
        }

        if (gDirtyTicker) {
          gDirtyTicker = false;
          buildTickerText(ticker, sizeof(ticker));
          tickerX = M5.Display.width();
        }
        drawTickerFrame(ticker);

      } else {
        // Boot/Offline表示（差分のみ）
        if (gDirtyStatus) {
          gDirtyStatus = false;
          drawBootDynamic();
        } else {
          // 時計だけは動かす（差分で）
          drawBootDynamic();
        }
      }

      lastUi = now;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void setup() {
  Serial.begin(115200);

  auto cfg = M5.config();
  M5.begin(cfg);

  // 表示の初動保証（あなたの参考コードに寄せる）
  M5.Speaker.end();
  M5.Display.setBrightness(255);
  M5.Display.setRotation(1);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.fillScreen(BLACK);
  M5.Display.setCursor(10, 10);
  M5.Display.setTextSize(2);
  M5.Display.print("Start");

  // mutex
  gMutex = xSemaphoreCreateMutex();

  // ティッカースプライト（滑らか化）
  tickerSpriteOK = tickerCanvas.createSprite(M5.Display.width(), 40);

  // タスク起動：UIはCore1、ネットはCore0推奨
  xTaskCreatePinnedToCore(UiTask,  "UiTask",  8192,  nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(NetTask, "NetTask", 8192,  nullptr, 1, nullptr, 0);

  // あなたの流儀：setup内常駐
  for(;;) {
    delay(1000);
  }
}

void loop() {
  // 仕様どおり未使用
}