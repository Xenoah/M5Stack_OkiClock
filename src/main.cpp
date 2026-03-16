#include <M5Unified.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <time.h>
#include <math.h>
#include <M5UnitENV.h>

#include "secrets.h"

// ===================== 設定 =====================
static const uint32_t BTC_UPDATE_MS   = 10 * 1000; // 10秒
static const uint32_t RSS_UPDATE_MS   = 60 * 1000; // 1分
static const uint32_t WIFI_TIMEOUT_MS = 15 * 1000;
static const uint32_t NTP_TIMEOUT_MS  = 8  * 1000;

static const uint32_t TOP_UI_MS          = 250;  // 上段更新
static const uint32_t NEWS_TICK_MS       = 33;   // ニューススクロール
static const int      SCROLL_PX_PER_TICK = 2;
static const int      TICKER_PX_PER_TICK = 6;    // 通貨ティッカー（高速）
static const uint32_t SENSOR_UPDATE_MS   = 2000; // センサー読み取り
static const uint32_t RATES_UPDATE_MS    = 5 * 60 * 1000; // 為替レート（5分）

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

static char     gTicker[512] = "(fetching rates...)";
static uint32_t gTickerRev   = 0;

// センサーデータ
static float    gTemp     = NAN;
static float    gHumid    = NAN;
static float    gPressure = NAN;
static int      gPage     = 0; // 0=メイン, 1=センサー

static SHT3X   gSht3x;
static QMP6988 gQmp6988;

// ===================== レイアウト（重なりゼロ） =====================
static const int TOP_H    = 72;
static const int TICKER_H = 20;
static const int TICKER_Y = TOP_H;           // = 72
static const int NEWS_Y   = TOP_H + TICKER_H; // = 92
static const int NEWS_H   = 240 - NEWS_Y;    // = 148 (4×37px)

// カラーパレット（ダークネイビー系）
static const uint16_t BG_TOP    = 0x0862; // 深い濃紺
static const uint16_t BG_PANEL  = 0x10C4; // やや明るいパネル
static const uint16_t BG_NEWS   = 0x0842; // ニュース背景（さらに暗く）
static const uint16_t C_ACCENT  = 0x05FA; // シアンアクセント
static const uint16_t C_DIM     = 0x3A8D; // 薄いテキスト

static const int LINE0_Y  = 0;
static const int LINE0_H  = 18;
static const int CLK_X    = 220; // 右上に時計
static const int BADGE_W  = 70;  // ニュースバッジ幅

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

// WiFiシグナル強度をバー数(0-4)に変換
static int rssiToBars(int rssi) {
  if (rssi >= -65) return 4;
  if (rssi >= -75) return 3;
  if (rssi >= -85) return 2;
  return 1;
}

// WiFiシグナルバー描画（スプライト用）
static void drawWifiBars(M5Canvas& c, int x, int y, int bars) {
  static const uint16_t BARCOLS[] = {C_DIM, 0xF800, 0xFD20, 0xFFE0, 0x07E0};
  uint16_t activeCol = BARCOLS[bars];
  for (int b = 0; b < 4; b++) {
    int bh = 6 + b * 3;
    int bx = x + b * 7;
    int by = y - bh;
    c.fillRect(bx, by, 5, bh, (b < bars) ? activeCol : C_DIM);
  }
}

// よく出るUnicode記号をASCII寄せ（BBCでも “ “ – … が混ざる）
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
// HTMLタグを除去（<letter や </ や <! で始まる場合のみタグとみなす）
// &lt; → < に展開された "5 < 6" のような文字は誤って削除しない
static void stripHtmlTags(String& s) {
  String out; out.reserve(s.length());
  bool inTag = false;
  for (size_t i = 0; i < s.length(); i++) {
    char c = s[i];
    if (!inTag && c == '<') {
      char nx = (i + 1 < s.length()) ? s[i + 1] : '\0';
      if (isalpha((unsigned char)nx) || nx == '/' || nx == '!') {
        inTag = true; continue; // 正規HTMLタグ → 除去
      }
      // それ以外の < は普通の文字として残す
    }
    if (inTag && c == '>')  { inTag = false; continue; }
    if (!inTag) out += c;
  }
  s = out;
}

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
            // 整形して追加（タグ除去→エンティティ展開→ASCII変換の順）
            stripHtmlTags(title);
            decodeEntities(title);
            stripHtmlTags(title);    // &lt;tag&gt; が展開された場合も除去
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

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  double v = doc["bitcoin"]["jpy"] | 0.0;
  if (v <= 0.0) return false;
  outBtc = v;
  return true;
}

// ===================== 為替レート =====================
// frankfurter.app: 必要な通貨だけリクエスト → 小さいJSON → getString で安全に取得
static const char* RATES_URL =
  "https://api.frankfurter.app/latest?from=USD&to=JPY,EUR,GBP,CNY,AUD,CAD,CHF,HKD,SGD,KRW,INR,MXN";

static bool fetchRates(char* dst, size_t dstsz) {
  WiFiClientSecure client;
  HTTPClient http;
  if (!httpsGetStream(RATES_URL, http, client)) return false;
  int code = http.GET();
  if (code != 200) { http.end(); return false; }
  String body = http.getString();
  http.end();
  if (body.length() == 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  static const struct { const char* code; int dec; } PAIRS[] = {
    {"JPY",1},{"EUR",4},{"GBP",4},{"CNY",4},{"AUD",4},
    {"CAD",4},{"CHF",4},{"HKD",4},{"SGD",4},{"KRW",0},
    {"INR",2},{"MXN",2},
  };
  static const int N = 12;

  String s; s.reserve(dstsz - 1);
  for (int i = 0; i < N; i++) {
    double rate = doc["rates"][PAIRS[i].code] | 0.0;
    if (rate <= 0.0) continue;
    if (s.length()) s += "   ";
    char buf[24];
    if      (PAIRS[i].dec == 0) snprintf(buf, sizeof(buf), "%s %.0f",  PAIRS[i].code, rate);
    else if (PAIRS[i].dec == 1) snprintf(buf, sizeof(buf), "%s %.1f",  PAIRS[i].code, rate);
    else if (PAIRS[i].dec == 2) snprintf(buf, sizeof(buf), "%s %.2f",  PAIRS[i].code, rate);
    else                        snprintf(buf, sizeof(buf), "%s %.4f",  PAIRS[i].code, rate);
    s += buf;
  }
  if (s.length() == 0) return false;
  snprintf(dst, dstsz, "%s", s.c_str());
  return true;
}

// ===================== UI：スプライト =====================
static M5Canvas topSpr   (&M5.Display); // 320x72  上段用
static M5Canvas tickerSpr(&M5.Display); // 320x20  通貨ティッカー
static M5Canvas newsSpr  (&M5.Display); // 250x37  ニュース1段分

// ティッカースクロール状態（UiTaskのみアクセス）
static String   tickerText          = "";
static int      tickerX             = 320;
static int      tickerW             = 0;
static uint32_t lastSeenTickerRev   = 0xFFFFFFFF;

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

  // バッジにカテゴリを表示するのでテキストにプレフィックス不要
  lines[0].text = String(wbuf);
  lines[1].text = String(bbuf);
  lines[2].text = String(tbuf);
  lines[3].text = firstItem(wbuf) + "  //  " + firstItem(bbuf) + "  //  " + firstItem(tbuf);

  lines[0].color = 0x07FF; // CYAN
  lines[1].color = 0xFD20; // ORANGE
  lines[2].color = 0xF81F; // MAGENTA
  lines[3].color = 0xFFFF; // WHITE

  newsSpr.setTextSize(2);
  for (int i = 0; i < 4; i++) {
    lines[i].w = newsSpr.textWidth(lines[i].text.c_str());
    if (lines[i].w < 1) lines[i].w = lines[i].text.length() * 12;
    lines[i].x = 250; // スプライト幅（右端から開始）
  }
}

static void drawTicker() {
  // データ更新チェック
  uint32_t rev;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  rev = gTickerRev;
  xSemaphoreGive(gMutex);

  if (rev != lastSeenTickerRev) {
    lastSeenTickerRev = rev;
    char buf[512];
    xSemaphoreTake(gMutex, portMAX_DELAY);
    snprintf(buf, sizeof(buf), "%s", gTicker);
    xSemaphoreGive(gMutex);
    tickerText = String(buf);
    tickerSpr.setTextSize(2);
    tickerW = tickerSpr.textWidth(tickerText.c_str());
    if (tickerW < 1) tickerW = tickerText.length() * 12;
    tickerX = 320;
  }

  tickerSpr.fillSprite(BG_PANEL);
  // 上下の細ライン（ティッカー感）
  tickerSpr.drawFastHLine(0, 0,           320, C_DIM);
  tickerSpr.drawFastHLine(0, TICKER_H - 1, 320, C_DIM);
  // スクロールテキスト
  tickerSpr.setTextSize(2);
  tickerSpr.setTextColor(C_ACCENT, BG_PANEL);
  tickerSpr.setCursor(tickerX, 2);
  tickerSpr.print(tickerText);
  tickerSpr.pushSprite(0, TICKER_Y);

  tickerX -= TICKER_PX_PER_TICK;
  if (tickerX < -tickerW) tickerX = 320;
}

static void drawStaticUI() {
  M5.Display.fillScreen(BG_TOP);

  // ニュースエリア背景
  M5.Display.fillRect(0, NEWS_Y, 320, NEWS_H, BG_NEWS);

  // アクセントライン
  M5.Display.drawFastHLine(0, TOP_H - 1, 320, C_ACCENT);

  // ニュースバッジエリア（静的：1回だけ描画）
  static const char* LABELS[]  = {"WORLD", "BIZ  ", "TECH ", "MIX  "};
  static const uint16_t BCOLS[] = {0x07FF, 0xFD20, 0xF81F, 0xFFFF}; // CYAN/ORANGE/MAGENTA/WHITE

  for (int i = 0; i < 4; i++) {
    int y = NEWS_Y + i * 37;
    M5.Display.fillRect(0, y, BADGE_W, 36, BG_PANEL);     // バッジ背景
    M5.Display.fillRect(0, y, 5,       36, BCOLS[i]);      // 左カラーバー
    M5.Display.drawFastVLine(BADGE_W, y, 36, C_DIM);       // 右境界線
    M5.Display.setTextSize(2);
    M5.Display.setTextColor(BCOLS[i], BG_PANEL);
    M5.Display.setCursor(9, y + 10);
    M5.Display.print(LABELS[i]);
  }
}

static void drawTopDynamic() {
  topSpr.fillSprite(BG_TOP);

  const bool wifiOk = (WiFi.status() == WL_CONNECTED);
  int rssi = wifiOk ? WiFi.RSSI() : 0;

  // ── ステータスバー (y=0..21) ──
  topSpr.fillRect(0, 0, 320, 22, BG_PANEL);

  // WiFiシグナルバー (x=4, 下端y=17)
  drawWifiBars(topSpr, 4, 17, wifiOk ? rssiToBars(rssi) : 0);

  // RSSI数値
  topSpr.setTextSize(1);
  topSpr.setTextColor(wifiOk ? 0x07E0 : 0xF800);
  topSpr.setCursor(34, 7);
  if (wifiOk) topSpr.printf("%ddBm", rssi);
  else        topSpr.print("--");

  // 時計（右上）
  topSpr.setTextSize(2);
  topSpr.setTextColor(WHITE);
  topSpr.setCursor(CLK_X, 3);
  topSpr.print(clockText());

  // ステータスバー下線
  topSpr.drawFastHLine(0, 21, 320, C_ACCENT);

  // ── 日付行 (y=22..43) ──
  topSpr.setTextSize(2);
  topSpr.setTextColor(C_DIM);
  topSpr.setCursor(4, 25);
  topSpr.print(dateTimeJST());

  // 日付下線
  topSpr.drawFastHLine(0, 43, 320, C_ACCENT);

  // ── BTC行 (y=44..71) ──
  topSpr.fillRect(0, 44, 320, 28, BG_PANEL);

  double btc, prev;
  uint32_t rev;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  btc = gBtc; prev = gBtcPrev; rev = gBtcRev;
  xSemaphoreGive(gMutex);

  uint16_t btcAccent = btcBorderColorFromChange(prev, btc);
  topSpr.fillRect(0, 44, 5, 28, btcAccent); // 左カラーバー

  char bline[56];
  if (btc > 0.0 && prev > 0.0) {
    double ch = (btc - prev) / prev * 100.0;
    char arrow = (ch >= 0) ? '^' : 'v';
    snprintf(bline, sizeof(bline), "BTC/JPY %c %.0f (%+.2f%%)", arrow, btc, ch);
  } else if (btc > 0.0) {
    snprintf(bline, sizeof(bline), "BTC/JPY  %.0f", btc);
  } else {
    snprintf(bline, sizeof(bline), "BTC/JPY  (pending)");
  }

  topSpr.setTextSize(2);
  topSpr.setTextColor(0xFFE0); // YELLOW
  topSpr.setCursor(10, 50);
  topSpr.print(bline);

  topSpr.pushSprite(0, 0);
  (void)rev;
}

static void drawNews4Lines() {
  const int lineH  = 37;
  const int startY = NEWS_Y;

  newsSpr.setTextWrap(false);
  newsSpr.setTextSize(2);

  for (int i = 0; i < 4; i++) {
    // 偶数行/奇数行で背景色を微妙に変えて視認性UP
    uint16_t rowBg = (i % 2 == 0) ? BG_NEWS : (uint16_t)(BG_NEWS + 0x0020);
    newsSpr.fillSprite(rowBg);
    newsSpr.setTextColor(lines[i].color, rowBg);
    newsSpr.setCursor(lines[i].x, 13); // 42px中央寄せ（(42-16)/2=13）
    newsSpr.print(lines[i].text);

    // バッジ右端（x=BADGE_W）からスプライトを押し出す
    newsSpr.pushSprite(BADGE_W, startY + i * lineH);

    lines[i].x -= SCROLL_PX_PER_TICK;
    if (lines[i].x < -lines[i].w) lines[i].x = 250;
  }
}

// ===================== センサーページ =====================
// 静的部分：ページ切替時に1回だけ呼ぶ
static void drawSensorPageStatic() {
  M5.Display.fillScreen(BG_TOP);

  // ヘッダー (y=0..23)
  M5.Display.fillRect(0, 0, 320, 23, BG_PANEL);
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(C_ACCENT, BG_PANEL);
  M5.Display.setCursor(84, 4);
  M5.Display.print("-- ENV III SENSOR --");
  M5.Display.drawFastHLine(0, 23, 320, C_ACCENT);

  // 温度セクション (y=24..95)
  M5.Display.fillRect(0, 24, 5, 72, 0xFD20);        // 左バー ORANGE
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(C_DIM, BG_TOP);
  M5.Display.setCursor(14, 30);
  M5.Display.print("TEMPERATURE");
  M5.Display.drawFastHLine(5, 95, 315, C_DIM);

  // 湿度セクション (y=96..167)
  M5.Display.fillRect(0, 96, 320, 72, BG_PANEL);
  M5.Display.fillRect(0, 96, 5, 72, 0x07E0);        // 左バー GREEN
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(C_DIM, BG_PANEL);
  M5.Display.setCursor(14, 102);
  M5.Display.print("HUMIDITY");
  // プログレスバー背景
  M5.Display.fillRect(14, 153, 294, 8, C_DIM);
  M5.Display.drawFastHLine(5, 167, 315, C_DIM);

  // 気圧セクション (y=168..239)
  M5.Display.fillRect(0, 168, 5, 72, 0xFFE0);       // 左バー YELLOW
  M5.Display.setTextSize(2);
  M5.Display.setTextColor(C_DIM, BG_TOP);
  M5.Display.setCursor(14, 174);
  M5.Display.print("PRESSURE");

  // フッターヒント
  M5.Display.setTextSize(1);
  M5.Display.setTextColor(C_DIM, BG_TOP);
  M5.Display.setCursor(220, 228);
  M5.Display.print("[C] Clock/News");
}

// 動的部分：値のみ上書き（fillScreen なし → ちらつきゼロ）
static void drawSensorPage() {
  float temp, humid, pressure;
  xSemaphoreTake(gMutex, portMAX_DELAY);
  temp = gTemp; humid = gHumid; pressure = gPressure;
  xSemaphoreGive(gMutex);

  // 温度値 (size4=32px高, y=50でセクション内中央)
  M5.Display.setTextSize(4);
  M5.Display.setTextColor(0xFD20, BG_TOP); // ORANGE
  M5.Display.setCursor(14, 50);
  if (!isnan(temp))   M5.Display.printf("  %5.1f C", temp);
  else                M5.Display.print ("    --- C");

  // 湿度値
  M5.Display.setTextColor(0x07E0, BG_PANEL); // GREEN
  M5.Display.setCursor(14, 118);
  if (!isnan(humid))  M5.Display.printf("  %5.1f %%", humid);
  else                M5.Display.print ("    --- %%");

  // 湿度プログレスバー
  int barW = (!isnan(humid) && humid > 0) ? (int)(humid / 100.0f * 294) : 0;
  if (barW > 294) barW = 294;
  M5.Display.fillRect(14,       153, barW,       8, 0x07E0); // GREEN
  M5.Display.fillRect(14 + barW, 153, 294 - barW, 8, C_DIM);

  // 気圧値
  M5.Display.setTextColor(0xFFE0, BG_TOP); // YELLOW
  M5.Display.setCursor(14, 192);
  if (!isnan(pressure)) M5.Display.printf(" %6.1f hPa", pressure);
  else                  M5.Display.print ("   ---  hPa");
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

  uint32_t lastBtc   = 0;
  uint32_t lastRss   = 0;
  uint32_t lastRates = RATES_UPDATE_MS; // 起動直後に1回フェッチ

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

    // 為替レート（5分ごと）
    if (now - lastRates >= RATES_UPDATE_MS) {
      char buf[512];
      if (fetchRates(buf, sizeof(buf))) {
        xSemaphoreTake(gMutex, portMAX_DELAY);
        snprintf(gTicker, sizeof(gTicker), "%s", buf);
        gTickerRev++;
        xSemaphoreGive(gMutex);
      }
      lastRates = now;
    }

    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

static void UiTask(void* arg){
  (void)arg;

  // スプライト生成
  topSpr.createSprite(320, TOP_H);    // 320x72 上段
  tickerSpr.createSprite(320, TICKER_H); // 320x20 通貨ティッカー
  newsSpr.createSprite(250, 37);      // 250x37 ニュース1段

  drawStaticUI();

  uint32_t lastTop        = 0;
  uint32_t lastNews       = 0;
  uint32_t lastSensor     = 0;
  uint32_t lastSensorDraw = 0;
  int      curPage        = -1; // 強制再描画トリガー

  for(;;){
    uint32_t now = millis();

    // ボタンC: ページ切替
    M5.update();
    if (M5.BtnC.wasPressed()) {
      xSemaphoreTake(gMutex, portMAX_DELAY);
      gPage = 1 - gPage;
      xSemaphoreGive(gMutex);
    }

    // 現在ページ取得
    int page;
    xSemaphoreTake(gMutex, portMAX_DELAY);
    page = gPage;
    xSemaphoreGive(gMutex);

    // ページ切替時に静的部分を一度だけ描画
    if (page != curPage) {
      curPage = page;
      if (page == 0) {
        drawStaticUI();
        lastTop = 0; lastNews = 0; // 即時更新
      } else {
        drawSensorPageStatic(); // fillScreen はここだけ
        lastSensorDraw = 0;     // 即時更新
      }
    }

    if (page == 0) {
      // ── メインページ ──
      if (now - lastTop >= TOP_UI_MS) {
        drawTopDynamic();
        lastTop = now;
      }
      if (now - lastNews >= NEWS_TICK_MS) {
        rebuild4LinesIfNeeded();
        drawTicker();
        drawNews4Lines();
        lastNews = now;
      }
    } else {
      // ── センサーページ ──
      // センサー読み取り
      if (now - lastSensor >= SENSOR_UPDATE_MS) {
        float t = NAN, h = NAN, p = NAN;
        if (gSht3x.update())   { t = gSht3x.cTemp;  h = gSht3x.humidity; }
        if (gQmp6988.update()) { p = gQmp6988.pressure / 100.0f; }
        xSemaphoreTake(gMutex, portMAX_DELAY);
        gTemp = t; gHumid = h; gPressure = p;
        xSemaphoreGive(gMutex);
        lastSensor = now;
      }
      // センサー描画（500ms毎）
      if (now - lastSensorDraw >= 500) {
        drawSensorPage();
        lastSensorDraw = now;
      }
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

  // ENV III センサー初期化
  Wire.begin(21, 22);
  gSht3x.begin(&Wire,   0x44, 21, 22, 400000UL);
  gQmp6988.begin(&Wire, 0x70, 21, 22, 400000UL);

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