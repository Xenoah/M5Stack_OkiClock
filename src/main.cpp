#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>

#include "secrets.h"

// ===================== 設定 =====================
static const uint32_t BTC_UPDATE_MS   = 10 * 1000; // 10秒
static const uint32_t RSS_UPDATE_MS   = 60 * 1000; // 1分
static const uint32_t WIFI_TIMEOUT_MS = 15 * 1000;
static const uint32_t NTP_TIMEOUT_MS  = 8  * 1000;

static const uint32_t TOP_UI_MS   = 250; // 上段更新
static const uint32_t NEWS_TICK_MS= 33;  // ニューススクロール
static const int      SCROLL_PX_PER_TICK = 2;

static const char* BTC_URL = "https://api.coingecko.com/api/v3/simple/price?ids=bitcoin&vs_currencies=jpy";

// BBC 3本（あなたの「取れてた時」と同じ URL体系）
static const char* RSS_WORLD_URL    = "https://feeds.bbci.co.uk/news/world/rss.xml";
static const char* RSS_BUSINESS_URL = "https://feeds.bbci.co.uk/news/business/rss.xml";
static const char* RSS_TECH_URL     = "https://feeds.bbci.co.uk/news/technology/rss.xml";

// バッファ（大きくしすぎると逆に危険。まずはこのくらい）
static const size_t RSS_BUF_SZ = 520;

// ===================== 共有状態（タスク間） =====================
static SemaphoreHandle_t gMutex;

static double   gBtc     = 0.0;
static double   gBtcPrev = 0.0;
static uint32_t gBtcRev  = 0;

static char     gWorld[RSS_BUF_SZ]    = "(pending)";
static char     gBusiness[RSS_BUF_SZ] = "(pending)";
static char     gTech[RSS_BUF_SZ]     = "(pending)";
static uint32_t gRssRev  = 0;

// ===================== レイアウト（重なりゼロ） =====================
static const int TOP_H   = 72;
static const int NEWS_Y  = TOP_H;
static const int NEWS_H  = 240 - TOP_H;

static const uint16_t BG_TOP  = BLACK;
static const uint16_t BG_NEWS = 0x3186; // 暗灰（黒ベタ回避）

// 0行目(18px)は「RSSI専用」で毎回塗り直す → 時刻混入対策
static const int LINE0_Y = 0;
static const int LINE0_H = 18;

static const int CLK_X = 225; // 右上に時計

// ===================== 便利関数 =====================
static bool isTimeValid() {
  time_t now = time(nullptr);
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
  uint32_t s = millis() / 1000;
  uint32_t hh = (s / 3600) % 100, mm = (s / 60) % 60, ss = s % 60;
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
  double ch = (now - prev) / prev;
  double mag = fabs(ch);
  float t = (float)(mag / 0.005); // 0.5%で最大
  if (t > 1.0f) t = 1.0f;
  return (ch >= 0.0) ? lerp565(30,30,30, 0,255,0, t) : lerp565(30,30,30, 255,0,0, t);
}

// よく出るUnicode記号をASCII寄せ（BBCでも “ ” – … が混ざる）
static void sanitizeUtf8ToAscii(String& s) {
  String out; out.reserve(s.length());
  auto match3 = [&](size_t i, uint8_t a, uint8_t b, uint8_t c) -> bool {
    return (i + 2 < s.length() &&
            (uint8_t)s[i] == a && (uint8_t)s[i+1] == b && (uint8_t)s[i+2] == c);
  };
  auto match2 = [&](size_t i, uint8_t a, uint8_t b) -> bool {
    return (i + 1 < s.length() && (uint8_t)s[i] == a && (uint8_t)s[i+1] == b);
  };
  for (size_t i = 0; i < s.length(); ) {
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) { out += (char)c; i++; continue; }
    if (match2(i, 0xC2, 0xA0)) { out += ' '; i += 2; continue; } // NBSP
    if (match3(i, 0xE2, 0x80, 0x98) || match3(i, 0xE2, 0x80, 0x99)) { out += '\''; i += 3; continue; }
    if (match3(i, 0xE2, 0x80, 0x9C) || match3(i, 0xE2, 0x80, 0x9D)) { out += '"';  i += 3; continue; }
    if (match3(i, 0xE2, 0x80, 0x93) || match3(i, 0xE2, 0x80, 0x94)) { out += '-';  i += 3; continue; }
    if (match3(i, 0xE2, 0x80, 0xA6)) { out += "..."; i += 3; continue; }
    int len = 1;
    if      ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    i += len;
    out += '?';
  }
  s = out;
}

// ===================== HTTP（BBCに強い設定：UA + identity） =====================
static bool httpsGetStream(const char* url, HTTPClient& http, WiFiClientSecure& client) {
  client.setInsecure();
  http.setTimeout(4500);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("Mozilla/5.0 (M5Stack; ESP32) RSSClient/1.0");
  http.useHTTP10(true);                 // ← chunked絡みの事故を減らす
  if (!http.begin(client, url)) return false;
  http.addHeader("Accept-Encoding", "identity");
  http.addHeader("Accept", "*/*");
  http.addHeader("Connection", "close");
  return true;
}

// ===================== RSS：ストリーム解析（XML丸ごと保持しない） =====================
static void decodeEntities(String& s) {
  s.replace("&amp;", "&");
  s.replace("&lt;", "<");
  s.replace("&gt;", ">");
  s.replace("&quot;", "\"");
  s.replace("&apos;", "'");
  s.replace("&#39;", "'");
}

static bool fetchRssTitlesStreamToBuf(const char* url, char* dst, size_t dstsz, int maxItems = 4) {
  if (!dst || dstsz == 0) return false;
  dst[0] = '\0';

  WiFiClientSecure client;
  HTTPClient http;

  if (!httpsGetStream(url, http, client)) return false;

  int code = http.GET();
  if (code != 200) { http.end(); return false; }

  WiFiClient* s = http.getStreamPtr();
  if (!s) { http.end(); return false; }

  // パターンマッチ
  const char* P_ITEM_S  = "<item";
  const char* P_ITEM_E  = "</item>";
  const char* P_TIT_S   = "<title>";
  const char* P_TIT_E   = "</title>";

  int miS=0, miE=0, mtS=0, mtE=0;
  bool inItem=false, inTitle=false;
  String title; title.reserve(180);
  String joined; joined.reserve(dstsz-1);
  int count=0;

  // inTitle中に "</title>" の判定をするための小バッファ
  String endbuf; endbuf.reserve(10);
  bool checkingEnd=false;

  while (s->connected() && s->available()) {
    char c = (char)s->read();

    // item開始/終了判定（title中でも動くが実害なし）
    miS = (c == P_ITEM_S[miS]) ? (miS+1) : (c == P_ITEM_S[0] ? 1 : 0);
    if (P_ITEM_S[miS] == '\0') { inItem = true; miS = 0; }

    miE = (c == P_ITEM_E[miE]) ? (miE+1) : (c == P_ITEM_E[0] ? 1 : 0);
    if (P_ITEM_E[miE] == '\0') { inItem = false; miE = 0; }

    // title開始判定（item内だけ）
    if (!inTitle && inItem) {
      mtS = (c == P_TIT_S[mtS]) ? (mtS+1) : (c == P_TIT_S[0] ? 1 : 0);
      if (P_TIT_S[mtS] == '\0') {
        inTitle = true;
        title = "";
        checkingEnd = false;
        endbuf = "";
        mtS = 0;
        continue; // "<title>" の '>' は本文に入れない
      }
    }

    if (inTitle) {
      // 終端タグ検出（途中で '<' が来たら終端候補扱い）
      if (!checkingEnd) {
        if (c == '<') {
          checkingEnd = true;
          endbuf = "<";
          mtE = 1;
          continue;
        } else {
          if (title.length() < 220) title += c;
          continue;
        }
      } else {
        // 終端候補の検証
        const char* pat = P_TIT_E;
        if (c == pat[mtE]) {
          endbuf += c;
          mtE++;
          if (pat[mtE] == '\0') {
            // </title> 完了
            inTitle = false;
            checkingEnd = false;
            mtE = 0;
            // 整形して追加
            decodeEntities(title);
            sanitizeUtf8ToAscii(title);
            title.trim();

            if (title.length()) {
              if (joined.length()) joined += "  |  ";
              joined += title;
              count++;
            }
            if (count >= maxItems) break;
            continue;
          }
          continue;
        } else {
          // 終端じゃなかった → endbufを本文に戻す
          checkingEnd = false;
          mtE = 0;
          if (title.length() + endbuf.length() < 240) title += endbuf;
          // 現在の文字も本文へ
          if (title.length() < 240) title += c;
          continue;
        }
      }
    }
  }

  http.end();

  if (count <= 0) return false;
  sanitizeUtf8ToAscii(joined);
  snprintf(dst, dstsz, "%s", joined.c_str());
  return true;
}

// ===================== BTC =====================
static bool fetchBtc(double& outBtc) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setTimeout(4500);
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setUserAgent("Mozilla/5.0 (M5Stack; ESP32) RSSClient/1.0");
  if (!http.begin(client, BTC_URL)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();

  StaticJsonDocument<256> doc;
  if (deserializeJson(doc, body)) return false;

  double v = doc["bitcoin"]["jpy"] | 0.0;
  if (v <= 0.0) return false;
  outBtc = v;
  return true;
}

// ===================== UI：ニュース4段（スプライト1枚使い回し） =====================
static M5Canvas newsSpr(&M5.Display); // 320x40 1枚のみ

struct NewsLine {
  String text;
  int x = 320;
  int w = 0;
  uint16_t color = WHITE;
};
static NewsLine lines[4];
static uint32_t lastSeenRssRev = 0;

static String firstItem(const char* s) {
  if (!s) return "";
  String ss = String(s);
  int p = ss.indexOf("  |  ");
  if (p < 0) return ss;
  return ss.substring(0, p);
}

static void rebuild4LinesIfNeeded() {
  uint32_t rev;
  char wbuf[RSS_BUF_SZ], bbuf[RSS_BUF_SZ], tbuf[RSS_BUF_SZ];

  xSemaphoreTake(gMutex, portMAX_DELAY);
  rev = gRssRev;
  snprintf(wbuf, sizeof(wbuf), "%s", gWorld);
  snprintf(bbuf, sizeof(bbuf), "%s", gBusiness);
  snprintf(tbuf, sizeof(tbuf), "%s", gTech);
  xSemaphoreGive(gMutex);

  if (rev == lastSeenRssRev) return;
  lastSeenRssRev = rev;

  lines[0].text = String("WORLD: ") + wbuf;
  lines[1].text = String("BUSINESS: ") + bbuf;
  lines[2].text = String("TECH: ") + tbuf;
  lines[3].text = String("MIX: ") + firstItem(wbuf) + "  |  " + firstItem(bbuf) + "  |  " + firstItem(tbuf);

  lines[0].color = CYAN;
  lines[1].color = ORANGE;
  lines[2].color = MAGENTA;
  lines[3].color = WHITE;

  newsSpr.setTextSize(3);
  for (int i=0;i<4;i++){
    lines[i].w = newsSpr.textWidth(lines[i].text.c_str());
    if (lines[i].w < 1) lines[i].w = lines[i].text.length() * 18;
    lines[i].x = M5.Display.width();
  }
}

static void drawStaticUI() {
  M5.Display.fillScreen(BG_TOP);
  M5.Display.fillRect(0, NEWS_Y, 320, NEWS_H, BG_NEWS);
  M5.Display.drawFastHLine(0, TOP_H-1, 320, 0x7BEF);

  // 上段ラベル
  M5.Display.setTextColor(WHITE, BG_TOP);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(0, 22); M5.Display.print("JST :");
  // BTC枠だけは静的枠を描かず、毎回描画側で描く（枠色が変わるため）
}

static void drawTopDynamic() {
  // 0行目は毎回塗り直す（RSSIに時計が混ざる問題を根絶）
  M5.Display.fillRect(0, LINE0_Y, 320, LINE0_H, BG_TOP);

  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  int rssi = wifiOk ? WiFi.RSSI() : 0;

  // RSSI表示（左側固定）
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(wifiOk ? GREEN : RED, BG_TOP);
  M5.Display.setCursor(0, 0);
  if (wifiOk) M5.Display.printf("WiFi OK RSSI:%ddBm", rssi);
  else        M5.Display.print("WiFi NG RSSI:--");

  // 時計（右上固定）
  M5.Display.setTextColor(WHITE, BG_TOP);
  M5.Display.setCursor(CLK_X, 0);
  M5.Display.print(clockText());

  // JST
  M5.Display.fillRect(70, 22, 250, 18, BG_TOP);
  M5.Display.setCursor(70, 22);
  M5.Display.setTextColor(WHITE, BG_TOP);
  M5.Display.print(dateTimeJST());

  // BTC（黄色文字＋枠色）
  double btc, prev;
  uint32_t rev;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  btc = gBtc; prev = gBtcPrev; rev = gBtcRev;
  xSemaphoreGive(gMutex);

  char bline[48];
  if (btc > 0.0 && prev > 0.0) {
    double ch = (btc - prev) / prev * 100.0;
    snprintf(bline, sizeof(bline), "BTC/JPY %.0f  (%+.2f%%)", btc, ch);
  } else if (btc > 0.0) {
    snprintf(bline, sizeof(bline), "BTC/JPY %.0f", btc);
  } else {
    snprintf(bline, sizeof(bline), "BTC/JPY (pending)");
  }

  uint16_t border = btcBorderColorFromChange(prev, btc);
  // BTC領域描画
  M5.Display.fillRect(0, 44, 320, 26, BG_TOP);
  M5.Display.drawRoundRect(0, 44, 320, 26, 6, border);
  M5.Display.setTextColor(YELLOW, BG_TOP);
  M5.Display.setTextSize(2);
  M5.Display.setCursor(8, 48);
  M5.Display.print(bline);

  (void)rev;
}

static void drawNews4Lines() {
  const int lineH = 40;
  const int startY = NEWS_Y + 4;

  newsSpr.setTextWrap(false);
  newsSpr.setTextSize(3);

  for (int i=0;i<4;i++){
    newsSpr.fillSprite(BG_NEWS);
    newsSpr.setTextColor(lines[i].color, BG_NEWS);
    newsSpr.setCursor(lines[i].x, 8);
    newsSpr.print(lines[i].text);

    newsSpr.pushSprite(0, startY + i*lineH);

    lines[i].x -= SCROLL_PX_PER_TICK;
    if (lines[i].x < -lines[i].w) lines[i].x = M5.Display.width();
  }
}

// ===================== タスク =====================
static void NetTask(void* arg){
  (void)arg;
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.begin(WIFI_SSID, WIFI_PASS);

  uint32_t wifiStart = millis();
  uint32_t ntpStart  = 0;
  bool ntpStarted = false;

  uint32_t lastBtc = 0;
  uint32_t lastRss = 0;

  for(;;){
    uint32_t now = millis();

    if (WiFi.status() != WL_CONNECTED) {
      if (now - wifiStart > WIFI_TIMEOUT_MS && now - wifiStart > 10000) {
        WiFi.disconnect(true, true);
        WiFi.begin(WIFI_SSID, WIFI_PASS);
        wifiStart = now;
      }
      vTaskDelay(pdMS_TO_TICKS(50));
      continue;
    }

    // NTP
    if (!isTimeValid() && !ntpStarted) {
      startNtpJST();
      ntpStart = now;
      ntpStarted = true;
    }
    if (ntpStarted && !isTimeValid() && (now - ntpStart > NTP_TIMEOUT_MS)) {
      ntpStarted = false; // 諦めても動かす
    }
    if (isTimeValid()) ntpStarted = false;

    // BTC
    if (now - lastBtc >= BTC_UPDATE_MS) {
      double v;
      if (fetchBtc(v)) {
        xSemaphoreTake(gMutex, portMAX_DELAY);
        gBtcPrev = (gBtc > 0.0) ? gBtc : v;
        gBtc = v;
        gBtcRev++;
        xSemaphoreGive(gMutex);
      }
      lastBtc = now;
    }

    // RSS（BBC 3本）
    if (now - lastRss >= RSS_UPDATE_MS) {
      char buf[RSS_BUF_SZ];

      bool okW = fetchRssTitlesStreamToBuf(RSS_WORLD_URL, buf, sizeof(buf), 4);
      xSemaphoreTake(gMutex, portMAX_DELAY);
      snprintf(gWorld, sizeof(gWorld), "%s", okW ? buf : "(WORLD failed)");
      xSemaphoreGive(gMutex);

      bool okB = fetchRssTitlesStreamToBuf(RSS_BUSINESS_URL, buf, sizeof(buf), 4);
      xSemaphoreTake(gMutex, portMAX_DELAY);
      snprintf(gBusiness, sizeof(gBusiness), "%s", okB ? buf : "(BUSINESS failed)");
      xSemaphoreGive(gMutex);

      bool okT = fetchRssTitlesStreamToBuf(RSS_TECH_URL, buf, sizeof(buf), 4);
      xSemaphoreTake(gMutex, portMAX_DELAY);
      snprintf(gTech, sizeof(gTech), "%s", okT ? buf : "(TECH failed)");
      gRssRev++;
      xSemaphoreGive(gMutex);

      lastRss = now;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void UiTask(void* arg){
  (void)arg;

  // ニューススプライトは「1枚だけ」
  newsSpr.createSprite(320, 40);

  drawStaticUI();

  uint32_t lastTop = 0;
  uint32_t lastNews= 0;

  for(;;){
    uint32_t now = millis();

    if (now - lastTop >= TOP_UI_MS) {
      drawTopDynamic();
      lastTop = now;
    }

    if (now - lastNews >= NEWS_TICK_MS) {
      rebuild4LinesIfNeeded();
      drawNews4Lines();
      lastNews = now;
    }

    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void setup(){
  Serial.begin(115200);
  auto cfg = M5.config();
  M5.begin(cfg);

  M5.Speaker.end();
  M5.Display.setBrightness(255);
  M5.Display.setRotation(1);
  M5.Display.setTextColor(WHITE, BLACK);

  gMutex = xSemaphoreCreateMutex();

  // 初期値
  xSemaphoreTake(gMutex, portMAX_DELAY);
  snprintf(gWorld, sizeof(gWorld), "%s", "(fetching...)");
  snprintf(gBusiness, sizeof(gBusiness), "%s", "(fetching...)");
  snprintf(gTech, sizeof(gTech), "%s", "(fetching...)");
  gRssRev++;
  xSemaphoreGive(gMutex);

  // UI=Core1、NET=Core0
  xTaskCreatePinnedToCore(UiTask,  "UiTask",  8192, nullptr, 2, nullptr, 1);
  xTaskCreatePinnedToCore(NetTask, "NetTask", 8192, nullptr, 1, nullptr, 0);

  for(;;) delay(1000);
}
void loop(){}