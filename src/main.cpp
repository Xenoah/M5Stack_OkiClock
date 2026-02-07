#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#include "secrets.h"

// ===================== 設定 =====================
static const uint32_t BTC_UPDATE_MS   = 10 * 1000;     // 10秒
static const uint32_t RSS_UPDATE_MS   = 60 * 1000;     // 1分
static const uint32_t WIFI_TIMEOUT_MS = 15 * 1000;
static const uint32_t NTP_TIMEOUT_MS  = 8  * 1000;

static const uint32_t TOP_UI_MS       = 250;           // 上段はゆっくり更新
static const uint32_t TICKER_MS       = 33;            // 下段 ticker は滑らかに
static const int      SCROLL_PX_PER_TICK = 2;

// Bitcoin (CoinGecko: simple price)
static const char* BTC_URL =
  "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=jpy";

// BBC RSS (primary: feeds.bbci.co.uk, fallback: newsrss.bbc.co.uk)
static const char* RSS_WORLD_PRIMARY   = "http://feeds.bbci.co.uk/news/world/rss.xml";
static const char* RSS_BUSINESS_PRIMARY= "http://feeds.bbci.co.uk/news/business/rss.xml";
static const char* RSS_TECH_PRIMARY    = "http://feeds.bbci.co.uk/news/technology/rss.xml";

static const char* RSS_WORLD_FALLBACK   = "http://newsrss.bbc.co.uk/rss/newsonline_uk_edition/world/rss.xml";
static const char* RSS_BUSINESS_FALLBACK= "http://newsrss.bbc.co.uk/rss/newsonline_uk_edition/business/rss.xml";
static const char* RSS_TECH_FALLBACK    = "http://newsrss.bbc.co.uk/rss/newsonline_uk_edition/technology/rss.xml";

// ===================== 共有状態（タスク間） =====================
static SemaphoreHandle_t g_lock;

static double   g_btc     = 0.0;
static double   g_btcPrev = 0.0;   // 前回値（変動率用）
static uint32_t g_btcRev  = 0;

static String   g_world, g_business, g_tech;
static uint32_t g_rssRev  = 0;

// ===================== 時刻ユーティリティ =====================
static bool isTimeValid() {
  time_t now = time(nullptr);
  // 2020-09-13 以降ならNTP同期済みっぽい扱い
  return (now > 1600000000);
}

static void startNtpJST() {
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org", "time.google.com");
}

static String clockText() {
  if (isTimeValid()) {
    struct tm tminfo;
    if (getLocalTime(&tminfo, 0)) {
      char buf[16];
      strftime(buf, sizeof(buf), "%H:%M:%S", &tminfo);
      return String(buf);
    }
  }
  // 未同期はuptimeを時計っぽく
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
  if (!isTimeValid()) return "----/--/-- --:--:--";
  struct tm tminfo;
  if (!getLocalTime(&tminfo, 0)) return "----/--/-- --:--:--";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M:%S", &tminfo);
  return String(buf);
}

// ===================== HTTP/取得 =====================
static bool httpsGetString(const char* url, String& out) {
  WiFiClientSecure client;
  client.setInsecure(); // 簡易運用（CA検証したいなら差し替え）

  HTTPClient http;
  http.setTimeout(4500);

  if (!http.begin(client, url)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  out = http.getString();
  http.end();
  return true;
}

static bool fetchBtcOnce(double& outBtc) {
  String body;
  if (!httpsGetString(BTC_URL, body)) return false;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) return false;

  double v = doc["bitcoin"]["jpy"] | 0.0;
  if (v <= 0.0) return false;

  outBtc = v;
  return true;
}

// RSSから <item> 内 <title> を拾う（超軽量パース）
static bool fetchRssTitlesFrom(const char* url, String& joined, int maxItems = 6) {
  String xml;
  if (!httpsGetString(url, xml)) return false;

  joined = "";
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

  return (count > 0);
}

static bool fetchRssWithFallback(const char* primary, const char* fallback, String& outJoined) {
  if (fetchRssTitlesFrom(primary, outJoined)) return true;
  return fetchRssTitlesFrom(fallback, outJoined);
}

// ===================== UI（差分描画） =====================
struct UiCache {
  char timeLine[16] = "";
  char wifiLine[32] = "";
  uint16_t wifiFg = 0xFFFF;

  char jstLine[32] = "";
  char btcLine[32] = "";
  uint16_t btcBorder = 0xFFFF;

  int tickerX = 0;
  int tickerTotalW = 0;

  uint32_t lastRssRev = 0;
  uint32_t lastBtcRev = 0;
};
static UiCache ui;

static const int TOP_H   = 72;
static const int NEWS_Y  = TOP_H;
static const int NEWS_H  = 240 - TOP_H;

static const uint16_t BG_TOP   = BLACK;
static const uint16_t BG_NEWS  = 0x2104; // かなり暗いグレー（黒ベタ感を弱める）
static const uint16_t FG_LABEL = WHITE;

static uint16_t lerp565(uint8_t r0, uint8_t g0, uint8_t b0,
                        uint8_t r1, uint8_t g1, uint8_t b1,
                        float t)
{
  if (t < 0) t = 0;
  if (t > 1) t = 1;
  uint8_t r = (uint8_t)(r0 + (r1 - r0) * t);
  uint8_t g = (uint8_t)(g0 + (g1 - g0) * t);
  uint8_t b = (uint8_t)(b0 + (b1 - b0) * t);
  return M5.Display.color565(r, g, b);
}

static uint16_t btcBorderColorFromChange(double prev, double now) {
  if (prev <= 0.0 || now <= 0.0) return WHITE;

  double ch = (now - prev) / prev;      // 変動率
  double mag = fabs(ch);

  // 0.5%で最大発色（好みで調整）
  float t = (float)(mag / 0.005);
  if (t > 1.0f) t = 1.0f;

  // ベースは暗め、上げ=緑、下げ=赤
  if (ch >= 0.0) return lerp565(40, 40, 40, 0, 255, 0, t);
  else           return lerp565(40, 40, 40, 255, 0, 0, t);
}

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

static void drawStaticUI() {
  M5.Display.fillScreen(BG_TOP);

  // 上段背景
  M5.Display.fillRect(0, 0, 320, TOP_H, BG_TOP);

  // 下段背景（黒ベタをやめる）
  M5.Display.fillRect(0, NEWS_Y, 320, NEWS_H, BG_NEWS);
  M5.Display.drawRect(0, NEWS_Y, 320, NEWS_H, 0x7BEF); // 薄い枠

  // ラベル
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(FG_LABEL, BG_TOP);

  M5.Display.setCursor(0, 0);
  M5.Display.print("WiFi:");

  M5.Display.setCursor(0, 22);
  M5.Display.print("JST :");

  // BTCパネル枠（中身は動的）
  M5.Display.drawRoundRect(0, 44, 320, 26, 6, WHITE);

  // 区切り線
  M5.Display.drawFastHLine(0, TOP_H - 1, 320, 0x7BEF);
}

// ticker 描画用 sprite
static M5Canvas tickerSpr(&M5.Display);

static void tickerRecalcTotalWidth(const String& w, const String& b, const String& t) {
  tickerSpr.setTextSize(3);
  String plain;
  plain.reserve(512);
  plain += "WORLD: ";    plain += (w.length() ? w : "(no data)");
  plain += "   |   ";
  plain += "BUSINESS: "; plain += (b.length() ? b : "(no data)");
  plain += "   |   ";
  plain += "TECH: ";     plain += (t.length() ? t : "(no data)");
  ui.tickerTotalW = tickerSpr.textWidth(plain.c_str());
  ui.tickerX = M5.Display.width();
}

static void drawTicker(const String& w, const String& b, const String& t) {
  const int tickerH = 70;
  const int y = NEWS_Y + (NEWS_H - tickerH) / 2;

  tickerSpr.fillSprite(BG_NEWS);
  tickerSpr.setTextSize(3);
  tickerSpr.setCursor(ui.tickerX, 18);

  // 色（カテゴリごと）
  const uint16_t C_WORLD = CYAN;
  const uint16_t C_BUS   = ORANGE;
  const uint16_t C_TECH  = MAGENTA;
  const uint16_t C_SEP   = 0xCE59; // 明るいグレー

  auto seg = [&](uint16_t col, const char* label, const String& body) {
    tickerSpr.setTextColor(col, BG_NEWS);
    tickerSpr.print(label);
    tickerSpr.print(": ");
    tickerSpr.print(body.length() ? body : String("(no data)"));
  };

  seg(C_WORLD, "WORLD", w);
  tickerSpr.setTextColor(C_SEP, BG_NEWS);
  tickerSpr.print("   |   ");
  seg(C_BUS, "BUSINESS", b);
  tickerSpr.setTextColor(C_SEP, BG_NEWS);
  tickerSpr.print("   |   ");
  seg(C_TECH, "TECH", t);

  tickerSpr.pushSprite(0, y);

  // スクロール
  ui.tickerX -= SCROLL_PX_PER_TICK;
  if (ui.tickerX < -ui.tickerTotalW) {
    ui.tickerX = M5.Display.width();
  }
}

static void drawTopDynamic() {
  // WiFi状態 / RSSI / 時計 / JST / BTC を必要箇所だけ更新
  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  const int rssi = wifiOk ? WiFi.RSSI() : 0;

  // 時計（左上の空きに大きめで）
  String clk = clockText();
  drawTextIfChanged(110, 0, 210, 18, BG_TOP, WHITE, 2, clk.c_str(), ui.timeLine, sizeof(ui.timeLine));

  // WiFi line (色付き)
  char wbuf[32];
  if (wifiOk) snprintf(wbuf, sizeof(wbuf), "OK  RSSI:%ddBm", rssi);
  else        snprintf(wbuf, sizeof(wbuf), "NG  RSSI:--");

  uint16_t wfg = wifiOk ? GREEN : RED;
  bool wifiForce = (ui.wifiFg != wfg);
  ui.wifiFg = wfg;

  drawTextIfChanged(70, 0, 240, 18, BG_TOP, wfg, 2, wbuf, ui.wifiLine, sizeof(ui.wifiLine), wifiForce);

  // JST datetime
  String dt = dateTimeJST();
  drawTextIfChanged(70, 22, 250, 18, BG_TOP, WHITE, 2, dt.c_str(), ui.jstLine, sizeof(ui.jstLine));

  // BTC panel
  double btc, prev;
  uint32_t btcRev;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  btc = g_btc;
  prev = g_btcPrev;
  btcRev = g_btcRev;
  xSemaphoreGive(g_lock);

  // 表示文字
  char bbuf[32];
  if (btc > 0.0 && prev > 0.0) {
    double ch = (btc - prev) / prev * 100.0;
    snprintf(bbuf, sizeof(bbuf), "BTC/JPY %.0f  (%+.2f%%)", btc, ch);
  } else if (btc > 0.0) {
    snprintf(bbuf, sizeof(bbuf), "BTC/JPY %.0f", btc);
  } else {
    snprintf(bbuf, sizeof(bbuf), "BTC/JPY (pending)");
  }

  uint16_t border = btcBorderColorFromChange(prev, btc);
  bool btcForce = (ui.btcBorder != border) || (ui.lastBtcRev != btcRev);
  ui.btcBorder = border;
  ui.lastBtcRev = btcRev;

  if (btcForce || strcmp(bbuf, ui.btcLine) != 0) {
    // 枠
    M5.Display.fillRect(0, 44, 320, 26, BG_TOP);
    M5.Display.drawRoundRect(0, 44, 320, 26, 6, border);

    // 文字（黄色）
    M5.Display.setTextColor(YELLOW, BG_TOP);
    M5.Display.setTextSize(2);
    M5.Display.setCursor(8, 48);
    M5.Display.print(bbuf);

    snprintf(ui.btcLine, sizeof(ui.btcLine), "%s", bbuf);
  }
}

// ===================== タスク =====================
static void TaskNet(void* arg) {
  (void)arg;

  uint32_t wifiStart = millis();
  uint32_t ntpStart = 0;
  bool ntpStarted = false;

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t lastBtc = 0;
  uint32_t lastRss = 0;

  for (;;) {
    uint32_t now = millis();

    // WiFi接続管理
    if (WiFi.status() != WL_CONNECTED) {
      if (now - wifiStart > WIFI_TIMEOUT_MS) {
        // 定期的に再トライ
        if (now - wifiStart > 10000) {
          WiFi.disconnect(true, true);
          WiFi.begin(WIFI_SSID, WIFI_PASS);
          wifiStart = now;
        }
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // NTP開始
    if (!isTimeValid() && !ntpStarted) {
      startNtpJST();
      ntpStart = now;
      ntpStarted = true;
    }
    if (ntpStarted && !isTimeValid() && (now - ntpStart > NTP_TIMEOUT_MS)) {
      // 諦めても動かす（uptime時計のまま）
      ntpStarted = false;
    }
    if (isTimeValid()) ntpStarted = false;

    // BTC
    if (now - lastBtc >= BTC_UPDATE_MS) {
      double v;
      if (fetchBtcOnce(v)) {
        xSemaphoreTake(g_lock, portMAX_DELAY);
        g_btcPrev = (g_btc > 0.0) ? g_btc : v; // 初回はprev=now
        g_btc = v;
        g_btcRev++;
        xSemaphoreGive(g_lock);
      }
      lastBtc = now;
    }

    // RSS（3本）
    if (now - lastRss >= RSS_UPDATE_MS) {
      String w, b, t;

      bool okW = fetchRssWithFallback(RSS_WORLD_PRIMARY,    RSS_WORLD_FALLBACK,    w);
      bool okB = fetchRssWithFallback(RSS_BUSINESS_PRIMARY, RSS_BUSINESS_FALLBACK, b);
      bool okT = fetchRssWithFallback(RSS_TECH_PRIMARY,     RSS_TECH_FALLBACK,     t);

      xSemaphoreTake(g_lock, portMAX_DELAY);
      g_world    = okW ? w : String("(world fetch failed)");
      g_business = okB ? b : String("(business fetch failed)");
      g_tech     = okT ? t : String("(tech fetch failed)");
      g_rssRev++;
      xSemaphoreGive(g_lock);

      lastRss = now;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void TaskUi(void* arg) {
  (void)arg;

  // ticker sprite 初期化
  tickerSpr.createSprite(320, 70);
  tickerSpr.setTextWrap(false);

  drawStaticUI();

  uint32_t lastTop = 0;
  uint32_t lastTick = 0;

  // ticker初期幅
  {
    String w, b, t;
    uint32_t rev;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    w = g_world; b = g_business; t = g_tech; rev = g_rssRev;
    xSemaphoreGive(g_lock);
    ui.lastRssRev = rev;
    tickerRecalcTotalWidth(w, b, t);
  }

  for (;;) {
    uint32_t now = millis();

    // 上段（必要な矩形だけ更新）
    if (now - lastTop >= TOP_UI_MS) {
      drawTopDynamic();
      lastTop = now;
    }

    // 下段 ticker（ここだけは毎フレーム描き直す）
    if (now - lastTick >= TICKER_MS) {
      String w, b, t;
      uint32_t rev;
      xSemaphoreTake(g_lock, portMAX_DELAY);
      w = g_world; b = g_business; t = g_tech; rev = g_rssRev;
      xSemaphoreGive(g_lock);

      if (rev != ui.lastRssRev) {
        ui.lastRssRev = rev;
        tickerRecalcTotalWidth(w, b, t);
      }

      drawTicker(w, b, t);
      lastTick = now;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ===================== setup / loop =====================
void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.setBrightness(255);
  M5.Display.setTextColor(WHITE, BLACK);

  g_lock = xSemaphoreCreateMutex();

  // 初期ダミー値
  xSemaphoreTake(g_lock, portMAX_DELAY);
  g_world = "(fetching...)";
  g_business = "(fetching...)";
  g_tech = "(fetching...)";
  xSemaphoreGive(g_lock);

  // タスク起動（NetはCore0、UIはCore1）
  xTaskCreatePinnedToCore(TaskNet, "net", 8192, nullptr, 1, nullptr, 0);
  xTaskCreatePinnedToCore(TaskUi,  "ui",  8192, nullptr, 2, nullptr, 1);

  // 仕様どおり loop は使わず、setup常駐
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void loop() {
  // 仕様どおり未使用
}