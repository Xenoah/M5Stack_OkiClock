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
static const uint32_t UI_TICK_MS    = 33;  // 約30fps
static const int      SCROLL_PX_PER_TICK = 2;

// ==== 状態 ====
double btc_jpy = 0.0;

String tickerText = "Loading...";
int tickerX = 0;

uint32_t lastBtc = 0;
uint32_t lastRss = 0;
uint32_t lastUi  = 0;

static void wifiConnect() {
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  M5.Display.print("WiFi");
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(200);
    M5.Display.print(".");
    if (millis() - t0 > 15000) break;
  }
  M5.Display.println(WiFi.status() == WL_CONNECTED ? " OK" : " NG");
}

static void syncTimeJST() {
  // JSTのTZ指定: "JST-9"
  configTzTime("JST-9", "ntp.nict.jp", "pool.ntp.org", "time.google.com");

  struct tm tminfo;
  for (int i = 0; i < 40; i++) {
    if (getLocalTime(&tminfo)) return;
    delay(200);
  }
}

static String nowJST() {
  struct tm tminfo;
  if (!getLocalTime(&tminfo)) return "----/--/-- --:--:--";
  char buf[32];
  strftime(buf, sizeof(buf), "%Y/%m/%d %H:%M:%S", &tminfo);
  return String(buf);
}

static bool httpsGetString(const char* url, String& out) {
  WiFiClientSecure client;
  client.setInsecure(); // 簡易運用。堅牢化するならCA検証へ。

  HTTPClient http;
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  out = http.getString();
  http.end();
  return true;
}

static bool fetchBtc() {
  String body;
  if (!httpsGetString(BTC_URL, body)) return false;

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) return false;

  btc_jpy = doc["bitcoin"]["jpy"] | 0.0;
  return btc_jpy > 0.0;
}

// RSSから <item> 内の <title> を最大N本拾う（超軽量パーサ）
static bool fetchRssTitles(String& joined, int maxItems = 5) {
  String xml;
  if (!httpsGetString(RSS_URL, xml)) return false;

  // channel直下titleを避けるため、<item>ごとに探す
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

  if (count == 0) return false;
  return true;
}

static void rebuildTicker() {
  // tickerText 例: "JST ...  BTC ...  NEWS ..."
  String s;
  s.reserve(256);

  s += "JST ";
  s += nowJST();
  s += "   BTC/JPY ";
  s += String((uint32_t)btc_jpy);
  s += "   NEWS ";
  s += tickerText; // tickerTextにRSS連結文字列を入れておく想定

  tickerText = s;

  // 画面右端から流し始める
  tickerX = M5.Display.width();
}

static void drawStaticArea() {
  M5.Display.setCursor(0, 0);
  M5.Display.setTextSize(2);
  M5.Display.printf("WiFi: %s\n", WiFi.status() == WL_CONNECTED ? "OK" : "NG");
  M5.Display.printf("JST : %s\n", nowJST().c_str());
  M5.Display.printf("BTC : %.0f JPY\n", btc_jpy);
  M5.Display.drawFastHLine(0, 78, M5.Display.width(), WHITE);
}

static void drawTicker() {
  // tickerエリア: y=84 付近
  const int y = 86;

  // クリア
  M5.Display.fillRect(0, y, M5.Display.width(), 40, BLACK);

  // 描画
  M5.Display.setCursor(tickerX, y);
  M5.Display.setTextSize(2);
  M5.Display.print(tickerText);

  // スクロール更新
  tickerX -= SCROLL_PX_PER_TICK;

  // 文字列幅をざっくり計測（M5GFXのtextWidthが使える場合はそれを使う）
  int approxW = tickerText.length() * 12; // TextSize=2の雑近似
  if (tickerX < -approxW) tickerX = M5.Display.width();
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Display.fillScreen(BLACK);
  M5.Display.setTextColor(WHITE, BLACK);
  M5.Display.setTextSize(2);

  wifiConnect();
  if (WiFi.status() == WL_CONNECTED) {
    syncTimeJST();
    fetchBtc();

    String titles;
    if (fetchRssTitles(titles)) {
      tickerText = titles;
    } else {
      tickerText = "(RSS fetch failed)";
    }
  } else {
    tickerText = "(No WiFi)";
  }

  rebuildTicker();

  lastBtc = millis();
  lastRss = millis();
  lastUi  = millis();

  // ここから常駐
  for(;;) {
    M5.update();
    uint32_t now = millis();

    // 更新: BTC
    if (WiFi.status() == WL_CONNECTED && now - lastBtc >= BTC_UPDATE_MS) {
      if (fetchBtc()) rebuildTicker();
      lastBtc = now;
    }

    // 更新: RSS
    if (WiFi.status() == WL_CONNECTED && now - lastRss >= RSS_UPDATE_MS) {
      String titles;
      if (fetchRssTitles(titles)) {
        // tickerTextは「RSS連結」を一旦保持してから rebuildTicker() で合成
        tickerText = titles;
        rebuildTicker();
      }
      lastRss = now;
    }

    // UI
    if (now - lastUi >= UI_TICK_MS) {
      M5.Display.fillScreen(BLACK);
      drawStaticArea();
      drawTicker();
      lastUi = now;
    }

    delay(1);
  }
}

void loop() {
  // 仕様どおり未使用
}