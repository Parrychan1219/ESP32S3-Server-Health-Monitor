#include <Arduino.h>
/*
 * ESP32-S3 Server Pinger
 * ----------------------
 *  - Pings the LAN server + the internet every cycle
 *  - HTTPS health-checks a list of subdomains every cycle
 *  - Timestamps + durations for WiFi/internet outages
 *  - Detects power outages across reboots via an NVS heartbeat
 *  - Telegram alerts (queued while offline, flushed on recovery)
 *  - Telegram commands: /status /check /reboot
 *  - Daily digest, OTA updates
 */

#include <WiFi.h>
#include <ESP32Ping.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include "secrets.h"

// ---------------------------------------------------------------- features
#define ENABLE_OTA          1   // flash over WiFi instead of unplugging the board
#define ENABLE_TG_COMMANDS  1   // reply to /status /check /reboot in Telegram
#define ENABLE_DAILY_DIGEST 1

#if ENABLE_OTA
  #include <ArduinoOTA.h>
#endif

// ---------------------------------------------------------------- credentials
const char* ssid = WIFI_SSID;
const char* pw   = WIFI_PASSWORD;

const String tg_token = TELEGRAM_BOT_TOKEN;
const String chatID   = TELEGRAM_CHAT_ID;

// ---------------------------------------------------------------- network
IPAddress local_IP(192, 168, 1, 220);
IPAddress gateway(192, 168, 1, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(1, 1, 1, 1);

IPAddress server_IP(192, 168, 1, 200);
IPAddress internet_IP1(1, 1, 1, 1);
IPAddress internet_IP2(8, 8, 8, 8);

// ---------------------------------------------------------------- SITES
// The list itself lives in sites.h - that is the only file you edit to add,
// remove or comment out a site. Each SITE(...) line there expands into one
// entry of this array.
struct SiteCfg { const char* name; const char* url; };

const SiteCfg SITES[] = {
  #define SITE(name, url) { name, url },
  #include "sites.h"
  #undef SITE
};
const uint8_t SITE_COUNT = sizeof(SITES) / sizeof(SITES[0]);

// ---------------------------------------------------------------- tuning
const unsigned long PING_INTERVAL   = 60000;  // main check cycle: 1 minute
const uint16_t  SITE_TIMEOUT_MS     = 8000;   // per-site HTTP timeout
const uint16_t  SITE_CONNECT_MS     = 6000;   // per-site TCP/TLS connect timeout
const uint8_t   SITE_FAIL_STRIKES   = 2;      // consecutive fails before "DOWN"
const uint8_t   SITE_OK_STRIKES     = 1;      // consecutive OKs before "UP"
const uint8_t   SERVER_FAIL_STRIKES = 3;      // unchanged from before

const unsigned long HEARTBEAT_MS    = 60000;  // NVS "still alive" write interval
const unsigned long WIFI_RETRY_MS   = 15000;  // reconnect attempt spacing
const unsigned long REBOOT_AFTER_MS = 900000; // reboot if WiFi dead 15 min
const unsigned long TG_POLL_MS      = 5000;   // Telegram command poll interval

#define TZ_INFO       "HKT-8"                 // POSIX TZ: Hong Kong, UTC+8, no DST
#define DIGEST_HOUR   9                       // daily report at 09:00 local
#define MIN_EPOCH     1700000000UL            // anything below this = clock unsynced

// ---------------------------------------------------------------- state
Preferences prefs;

enum DeviceState {
  WIFI_DOWN,
  NO_INTERNET_WIFI_OK,
  SERVER_UP,
  SERVER_MISSED_LATEST,
  SERVER_DEAD_INTERNET_UP,
  SITE_DOWN_SERVER_UP,
  WIFI_CONNECTED_IDLE,
  OTA_ACTIVE
};
DeviceState currentState = WIFI_DOWN;

struct SiteState {
  bool     up;
  uint8_t  fails;
  uint8_t  oks;
  time_t   firstFailAt;
  uint32_t firstFailMs;
  uint32_t checks;
  uint32_t okChecks;
  int      lastCode;
  uint32_t lastMs;
};
SiteState siteState[SITE_COUNT];

unsigned long lastPingTime      = 0;
unsigned long lastHeartbeatMs   = 0;
unsigned long lastWifiTry       = 0;
unsigned long lastTgPoll        = 0;
unsigned long flashTimer        = 0;
bool          flashState        = false;
bool          firstCycleDone    = false;

int      failedServerPings = 0;
bool     serverUp          = true;
time_t   serverDownAt      = 0;
uint32_t serverDownAtMs    = 0;

// connectivity outage bookkeeping
bool     netDown        = false;
bool     netDownMsValid = false;
time_t   netDownAt      = 0;
uint32_t netDownAtMs    = 0;
String   netDownReason  = "";
uint32_t outageCount    = 0;
uint32_t outageTotalSec = 0;

// boot info
esp_reset_reason_t bootReason;
uint32_t bootCount      = 0;
time_t   lastSeenAtBoot = 0;
time_t   pendingOutage  = 0;
int      lastDigestDay  = -1;
bool     bootReportPending = true;
bool     otaStarted        = false;
volatile bool otaInProgress   = false;

// S3 built-in RGB LED
#ifndef RGB_BUILTIN
#define RGB_BUILTIN 48
#endif
uint32_t lastRGBWritten = 0xFFFFFFFF;

// ================================================================ helpers

time_t nowEpoch() { return time(nullptr); }
bool   clockOK()  { return nowEpoch() > (time_t)MIN_EPOCH; }

String fmtTime(time_t t) {
  if (t < (time_t)MIN_EPOCH) return "unknown (clock not synced)";
  struct tm ti;
  localtime_r(&t, &ti);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &ti);
  return String(buf);
}

String fmtDur(uint32_t sec) {
  if (sec == 0) return "under a second";
  uint32_t d = sec / 86400; sec %= 86400;
  uint32_t h = sec / 3600;  sec %= 3600;
  uint32_t m = sec / 60;
  uint32_t s = sec % 60;
  String o;
  if (d)      o += String(d) + "d ";
  if (h || d) o += String(h) + "h ";
  if (m || h || d) o += String(m) + "m ";
  o += String(s) + "s";
  return o;
}

String urlEncode(const String &in) {
  static const char* hex = "0123456789ABCDEF";
  String out;
  out.reserve(in.length() * 3);
  for (size_t i = 0; i < in.length(); i++) {
    uint8_t c = (uint8_t)in[i];
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
      out += (char)c;
    } else {
      out += '%';
      out += hex[(c >> 4) & 0x0F];
      out += hex[c & 0x0F];
    }
  }
  return out;
}

const char* resetReasonStr(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "power-on (cold boot / mains restored)";
    case ESP_RST_BROWNOUT:  return "brownout (supply voltage dipped)";
    case ESP_RST_EXT:       return "external reset pin";
    case ESP_RST_SW:        return "software restart";
    case ESP_RST_PANIC:     return "crash / panic";
    case ESP_RST_INT_WDT:   return "interrupt watchdog";
    case ESP_RST_TASK_WDT:  return "task watchdog";
    case ESP_RST_WDT:       return "watchdog";
    case ESP_RST_DEEPSLEEP: return "deep sleep wake";
    default:                return "unknown";
  }
}

// ================================================================ LEDs

void writeRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  if (v == lastRGBWritten) return;
  lastRGBWritten = v;
  neopixelWrite(RGB_BUILTIN, r, g, b);
}

void setLED(uint8_t r, uint8_t g, uint8_t b) {
  // Scale down heavily to absolute single digits because the hardware curve is clapped
  uint8_t dim_r = r > 0 ? 8 : 0;
  uint8_t dim_g = g > 0 ? 8 : 0;
  uint8_t dim_b = b > 0 ? 8 : 0;

  if (r == 128 && b == 128) { dim_r = 4; dim_b = 4; }   // purple
  if (r == 255 && g == 255) { dim_r = 4; dim_g = 4; }   // yellow

  if (currentState == SERVER_DEAD_INTERNET_UP) {
    writeRGB(r, g, b);          // full brightness, this one matters
  } else {
    writeRGB(dim_r, dim_g, dim_b);
  }
}

void flash(uint8_t r, uint8_t g, uint8_t b, uint16_t period) {
  if (millis() - flashTimer > period) {
    flashTimer = millis();
    flashState = !flashState;
  }
  if (flashState) setLED(r, g, b);
  else            setLED(0, 0, 0);
}

void handleLEDs() {
  switch (currentState) {
    case WIFI_DOWN:               setLED(255, 0, 0);          break; // red
    case WIFI_CONNECTED_IDLE:     setLED(128, 0, 128);        break; // purple
    case NO_INTERNET_WIFI_OK:     flash(128, 0, 128, 500);    break; // purple flash
    case SERVER_UP:               setLED(0, 255, 0);          break; // green
    case SERVER_MISSED_LATEST:    setLED(255, 255, 0);        break; // yellow
    case SERVER_DEAD_INTERNET_UP: flash(255, 0, 0, 500);      break; // red flash
    case SITE_DOWN_SERVER_UP:     flash(0, 0, 255, 800);      break; // blue flash
    case OTA_ACTIVE:              flash(0, 0, 255, 120);      break; // fast blue = flashing
  }
}

// ================================================================ Telegram

bool sendTelegramNow(const String &message) {
  if (WiFi.status() != WL_CONNECTED) return false;

  WiFiClientSecure client;
  client.setInsecure();                 // ignore SSL certs

  HTTPClient http;
  http.setConnectTimeout(8000);
  http.setTimeout(10000);

  String url = "https://api.telegram.org/bot" + tg_token +
               "/sendMessage?chat_id=" + chatID +
               "&disable_web_page_preview=true&text=" + urlEncode(message);

  if (!http.begin(client, url)) {
    Serial.println("[-] Unable to initialize secure client.");
    return false;
  }
  int httpCode = http.GET();
  http.end();

  if (httpCode == 200) {
    Serial.println("[+] Telegram delivered.");
    return true;
  }
  Serial.printf("[-] Telegram failed, code %d\n", httpCode);
  return false;
}

// Ring buffer so alerts raised while offline are not lost.
#define ALERT_QUEUE_SIZE 12
String  alertQueue[ALERT_QUEUE_SIZE];
uint8_t qHead = 0, qCount = 0;

void notify(const String &msg) {
  Serial.println("---- TELEGRAM ----\n" + msg + "\n------------------");
  if (sendTelegramNow(msg)) return;

  if (qCount == ALERT_QUEUE_SIZE) {          // drop the oldest
    qHead = (qHead + 1) % ALERT_QUEUE_SIZE;
    qCount--;
  }
  alertQueue[(qHead + qCount) % ALERT_QUEUE_SIZE] = msg;
  qCount++;
  Serial.printf("[*] Queued alert (%u pending).\n", qCount);
}

void flushAlertQueue() {
  while (qCount > 0) {
    if (!sendTelegramNow(alertQueue[qHead])) return;   // still offline, try later
    alertQueue[qHead] = "";
    qHead = (qHead + 1) % ALERT_QUEUE_SIZE;
    qCount--;
    delay(250);                                        // be nice to the API
  }
}

// ================================================================ status text

String buildStatus() {
  String s;
  s += "Time:    " + fmtTime(nowEpoch()) + "\n";
  s += "Uptime:  " + fmtDur(millis() / 1000) + "\n";
  s += "Boots:   " + String(bootCount) + " (last: " + resetReasonStr(bootReason) + ")\n";
  s += "Outages: " + String(outageCount) + " since boot, " + fmtDur(outageTotalSec) + " total\n";
  s += "WiFi:    " + String(WiFi.RSSI()) + " dBm, IP " + WiFi.localIP().toString() + "\n";
  s += "Heap:    " + String(ESP.getFreeHeap() / 1024) + " kB free\n\n";

  s += "Server " + server_IP.toString() + ": " + String(serverUp ? "UP" : "DOWN");
  if (!serverUp) s += " since " + fmtTime(serverDownAt);
  s += "\n\nSites:\n";

  for (uint8_t i = 0; i < SITE_COUNT; i++) {
    SiteState &st = siteState[i];
    s += String(st.up ? "  [UP]   " : "  [DOWN] ") + SITES[i].name;
    if (st.checks) {
      uint32_t pct = (st.okChecks * 1000UL) / st.checks;   // one decimal
      s += "  " + String(pct / 10) + "." + String(pct % 10) + "%";
    }
    if (st.lastCode > 0) s += "  HTTP " + String(st.lastCode) + " / " + String(st.lastMs) + "ms";
    else                 s += "  no response";
    if (!st.up) s += "\n          down since " + fmtTime(st.firstFailAt);
    s += "\n";
  }
  return s;
}

// ================================================================ checks

bool checkInternet() {
  return Ping.ping(internet_IP1, 1) || Ping.ping(internet_IP2, 1);
}

// Returns true if the site answered with a status below 500.
bool httpCheck(const char* url, int &code, uint32_t &elapsedMs) {
  WiFiClientSecure client;
  client.setInsecure();
  client.setTimeout(SITE_TIMEOUT_MS / 1000);

  HTTPClient http;
  http.setConnectTimeout(SITE_CONNECT_MS);
  http.setTimeout(SITE_TIMEOUT_MS);
  http.setUserAgent("ESP32-Pinger/2.0");
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setRedirectLimit(3);

  uint32_t t0 = millis();
  if (!http.begin(client, url)) {
    code = 0;
    elapsedMs = millis() - t0;
    return false;
  }
  code = http.GET();
  elapsedMs = millis() - t0;
  http.end();

  return (code > 0 && code < 500);
}

// ================================================================ outage tracking

void beginNetOutage(const String &reason) {
  if (netDown) return;
  netDown        = true;
  netDownMsValid = true;
  netDownAt      = nowEpoch();
  netDownAtMs    = millis();
  netDownReason  = reason;
  outageCount++;

  // Persist so a reboot mid-outage does not lose the start time.
  prefs.putULong("outSince", clockOK() ? (uint32_t)netDownAt : 0);

  Serial.println("[!] Connectivity lost: " + reason + " at " + fmtTime(netDownAt));
}

void endNetOutage() {
  if (!netDown) return;

  uint32_t dur = 0;
  if (netDownMsValid)                          dur = (millis() - netDownAtMs) / 1000;
  else if (clockOK() && netDownAt > (time_t)MIN_EPOCH) dur = (uint32_t)(nowEpoch() - netDownAt);

  netDown = false;
  outageTotalSec += dur;
  prefs.putULong("outSince", 0);

  String m  = "INTERNET RESTORED\n\n";
         m += "Went down:  " + fmtTime(netDownAt) + "\n";
         m += "Came back:  " + fmtTime(nowEpoch()) + "\n";
         m += "Offline for: " + fmtDur(dur) + "\n";
         m += "Cause seen: " + netDownReason + "\n\n";
         m += "Connection is working properly again.";
  notify(m);
}

void heartbeat() {
  if (millis() - lastHeartbeatMs < HEARTBEAT_MS) return;
  lastHeartbeatMs = millis();
  if (clockOK()) prefs.putULong("lastSeen", (uint32_t)nowEpoch());
}

// ================================================================ boot report

void sendBootReport() {
  bool powerLoss = (bootReason == ESP_RST_POWERON || bootReason == ESP_RST_BROWNOUT);

  // Back-compute when this boot actually happened. The router often takes
  // minutes longer than the ESP32 to come back after a power cut, so "now"
  // would overstate the outage.
  time_t bootedAt = clockOK() ? (time_t)(nowEpoch() - (millis() / 1000)) : 0;

  String m  = "PINGER ONLINE\n\n";
         m += "Boot #" + String(bootCount) + "\n";
         m += "Reset reason: " + String(resetReasonStr(bootReason)) + "\n";
         m += "Time now: " + fmtTime(nowEpoch()) + "\n";

  if (powerLoss) {
    if (lastSeenAtBoot > (time_t)MIN_EPOCH && bootedAt > lastSeenAtBoot) {
      uint32_t gap = (uint32_t)(bootedAt - lastSeenAtBoot);
      m += "\nPOWER OUTAGE DETECTED\n";
      m += "Last seen alive: " + fmtTime(lastSeenAtBoot) + "\n";
      m += "Power restored:  " + fmtTime(bootedAt) + "\n";
      m += "No power for:    ~" + fmtDur(gap) +
           "  (+/- " + String(HEARTBEAT_MS / 1000) + "s)\n";
      m += "\nBoard is powered and monitoring again.";
    } else {
      m += "\nNo previous heartbeat stored - treating this as a fresh install.";
    }
  } else if (pendingOutage > (time_t)MIN_EPOCH) {
    m += "\nRebooted while the connection was down (outage began " +
         fmtTime(pendingOutage) + ").";
  }

  m += "\n\nWatching " + String(SITE_COUNT) + " site(s) + server " + server_IP.toString() +
       ", every " + String(PING_INTERVAL / 1000) + "s.";
  notify(m);
}

// ================================================================ Telegram commands

#if ENABLE_TG_COMMANDS
long tgOffset = 0;

String helpText() {
  return
    "Server Pinger - available commands\n"
    "\n"
    "/status\n"
    "    Current state: uptime, boot count, outage totals, per-site\n"
    "    availability, WiFi signal and free heap.\n"
    "\n"
    "/check\n"
    "    Run a check cycle immediately instead of waiting for the\n"
    "    next one, which can be up to a minute away.\n"
    "\n"
    "/reboot\n"
    "    Restart the board. It reports back once it is up again.\n"
    "\n"
    "/help\n"
    "    This message.\n"
    "\n"
    "Alerts are sent on their own when a site or the server goes down\n"
    "or recovers, when the internet drops, and after a power cut.";
}

void handleCommand(const String &cmd) {
  Serial.println("[*] Command: " + cmd);

  if (cmd.startsWith("/status") || cmd.startsWith("/uptime")) {
    notify("STATUS\n\n" + buildStatus());
  } else if (cmd.startsWith("/check")) {
    notify("Running a check cycle now...");
    lastPingTime = millis() - PING_INTERVAL; // force the next loop() to run one
  } else if (cmd.startsWith("/reboot")) {
    notify("Rebooting on request.");
    delay(500);
    ESP.restart();
  } else if (cmd.startsWith("/help") || cmd.startsWith("/start")) {
    // /start is what Telegram sends when the chat is first opened.
    notify(helpText());
  } else {
    // Anything else, slash-prefixed or not, is not something we know.
    String shown = cmd;
    if (shown.length() > 40) shown = shown.substring(0, 40) + "...";
    notify("Not a command: \"" + shown + "\"\n\nSend /help to see what I understand.");
  }
}

void handleTelegramUpdates(const String &body) {
  int idx = 0;
  while (true) {
    int u = body.indexOf("\"update_id\":", idx);
    if (u < 0) break;

    int numEnd = body.indexOf(',', u + 12);
    if (numEnd < 0) break;
    long id = body.substring(u + 12, numEnd).toInt();
    if (id >= tgOffset) tgOffset = id + 1;

    int next  = body.indexOf("\"update_id\":", u + 12);
    int limit = (next < 0) ? body.length() : next;
    String chunk = body.substring(u, limit);

    // only obey messages from the configured chat
    if (chunk.indexOf("\"id\":" + chatID) >= 0) {
      int t = chunk.indexOf("\"text\":\"");
      if (t >= 0) {
        int e = chunk.indexOf('"', t + 8);
        if (e > t) {
          String txt = chunk.substring(t + 8, e);
          txt.trim();
          txt.toLowerCase();
          handleCommand(txt);
        }
      } else {
        // A sticker, photo or voice note. allowed_updates keeps this to
        // real messages, so there is nothing else this could be.
        Serial.println("[*] Non-text message from owner");
        notify("Not a command: that message has no text.\n\nSend /help to see what I understand.");
      }
    }

    if (next < 0) break;
    idx = limit;
  }
}

void pollTelegram() {
  if (millis() - lastTgPoll < TG_POLL_MS) return;
  lastTgPoll = millis();

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);

  String url = "https://api.telegram.org/bot" + tg_token +
               "/getUpdates?timeout=0&limit=5"
               "&allowed_updates=%5B%22message%22%5D";
  if (tgOffset) url += "&offset=" + String(tgOffset);

  if (!http.begin(client, url)) return;
  int code = http.GET();
  if (code == 200) {
    String body = http.getString();
    http.end();
    handleTelegramUpdates(body);
  } else {
    http.end();
  }
}
#endif  // ENABLE_TG_COMMANDS

// ================================================================ daily digest

#if ENABLE_DAILY_DIGEST
void maybeDailyDigest() {
  if (!clockOK()) return;
  time_t t = nowEpoch();
  struct tm ti;
  localtime_r(&t, &ti);

  if (lastDigestDay < 0) {                    // first sync after boot
    lastDigestDay = (ti.tm_hour >= DIGEST_HOUR) ? ti.tm_yday : -2;
    return;
  }
  if (ti.tm_hour == DIGEST_HOUR && ti.tm_yday != lastDigestDay) {
    lastDigestDay = ti.tm_yday;
    notify("DAILY REPORT\n\n" + buildStatus());
  }
}
#endif

// ================================================================ WiFi

void handleWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;

  currentState = WIFI_DOWN;
  beginNetOutage("WiFi lost association with \"" + String(ssid) + "\"");

  if (millis() - lastWifiTry > WIFI_RETRY_MS) {
    lastWifiTry = millis();
    Serial.println("[*] WiFi reconnect attempt...");
    WiFi.disconnect();
    WiFi.begin(ssid, pw);
  }

  if (netDownMsValid && millis() - netDownAtMs > REBOOT_AFTER_MS) {
    Serial.println("[!] WiFi down too long. Rebooting.");
    if (clockOK()) prefs.putULong("outSince", (uint32_t)netDownAt);
    delay(200);
    ESP.restart();
  }
}

void syncTime(uint16_t waitMs) {
  configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  uint32_t t0 = millis();
  while (!clockOK() && millis() - t0 < waitMs) delay(200);
  Serial.println(clockOK() ? "[+] Clock synced: " + fmtTime(nowEpoch())
                           : "[-] NTP sync failed.");
}

// ================================================================ check cycle

void checkSites(bool &anySiteDown) {
  anySiteDown = false;

  for (uint8_t i = 0; i < SITE_COUNT; i++) {
    SiteState &st = siteState[i];
    int code = 0;
    uint32_t ms = 0;

    bool ok = httpCheck(SITES[i].url, code, ms);
    st.checks++;
    st.lastCode = code;
    st.lastMs   = ms;
    if (ok) st.okChecks++;

    Serial.printf("[%s] %s -> code %d (%lu ms)\n",
                  ok ? "+" : "-", SITES[i].name, code, (unsigned long)ms);

    if (ok) {
      st.fails = 0;
      st.oks++;
      if (!st.up && st.oks >= SITE_OK_STRIKES) {
        st.up = true;
        uint32_t dur = st.firstFailMs ? (millis() - st.firstFailMs) / 1000 : 0;
        String m  = "SITE UP: " + String(SITES[i].name) + "\n\n";
               m += String(SITES[i].url) + "\n";
               m += "Recovered: " + fmtTime(nowEpoch()) + "\n";
               m += "Went down: " + fmtTime(st.firstFailAt) + "\n";
               m += "Down for:  " + fmtDur(dur) + "\n";
               m += "Now answering HTTP " + String(code) + " in " + String(ms) + " ms.";
        notify(m);
      }
    } else {
      st.oks = 0;
      st.fails++;
      if (st.fails == 1) {                    // remember when it first misbehaved
        st.firstFailAt = nowEpoch();
        st.firstFailMs = millis();
      }
      if (st.up && st.fails >= SITE_FAIL_STRIKES) {
        st.up = false;
        String why = (code > 0) ? ("HTTP " + String(code))
                                : "no response / connection refused / TLS failure";
        String m  = "SITE DOWN: " + String(SITES[i].name) + "\n\n";
               m += String(SITES[i].url) + "\n";
               m += "First failed: " + fmtTime(st.firstFailAt) + "\n";
               m += "Confirmed:    " + fmtTime(nowEpoch()) + " after " +
                    String(st.fails) + " tries\n";
               m += "Reason: " + why;
        notify(m);
      }
    }

    if (!st.up) anySiteDown = true;
    delay(50);                                 // let the TLS context tear down
  }
}

void runCheckCycle() {
  Serial.println("\n[*] ===== check cycle =====");

  Serial.println("[*] Pinging Internet...");
  if (!checkInternet()) {
    beginNetOutage("WiFi associated but 1.1.1.1 and 8.8.8.8 both unreachable");
    currentState = NO_INTERNET_WIFI_OK;
    return;
  }
  endNetOutage();                              // sends the recovery message if needed

  if (!clockOK()) syncTime(5000);

  // ---- LAN server ----
  Serial.println("[*] Pinging Server...");
  if (Ping.ping(server_IP, 1)) {
    if (failedServerPings >= SERVER_FAIL_STRIKES) {
      uint32_t dur = serverDownAtMs ? (millis() - serverDownAtMs) / 1000 : 0;
      String m  = "SERVER UP: mainhomeserver\n\n";
             m += "Recovered: " + fmtTime(nowEpoch()) + "\n";
             m += "Went down: " + fmtTime(serverDownAt) + "\n";
             m += "Down for:  " + fmtDur(dur);
      notify(m);
    }
    failedServerPings = 0;
    serverUp = true;
    currentState = SERVER_UP;
    Serial.println("[+] Server is alive.");
  } else {
    failedServerPings++;
    Serial.printf("[-] Server missed ping. Strike %d\n", failedServerPings);
    if (failedServerPings == 1) {
      serverDownAt   = nowEpoch();
      serverDownAtMs = millis();
    }
    if (failedServerPings < SERVER_FAIL_STRIKES) {
      currentState = SERVER_MISSED_LATEST;
    } else {
      currentState = SERVER_DEAD_INTERNET_UP;
      if (failedServerPings == SERVER_FAIL_STRIKES) {
        serverUp = false;
        String m  = "SERVER DOWN: mainhomeserver\n\n";
               m += String(SERVER_FAIL_STRIKES) + " consecutive pings missed.\n";
               m += "First miss: " + fmtTime(serverDownAt) + "\n";
               m += "Confirmed:  " + fmtTime(nowEpoch());
        notify(m);
      }
    }
  }

  // ---- public sites ----
  bool anySiteDown = false;
  if (SITE_COUNT) {
    Serial.println("[*] Checking sites...");
    checkSites(anySiteDown);
  }

  // site trouble only colours the LED when the LAN server itself looks fine
  if (anySiteDown && currentState == SERVER_UP) currentState = SITE_DOWN_SERVER_UP;

  firstCycleDone = true;
  Serial.println("[*] ===== cycle done =====\n");
}

// ================================================================ boot tasks

#if ENABLE_OTA
// loop() can block for 20-30s while HTTP checks time out - long enough for
// espota.py to give up waiting on the handshake, and that is exactly when you
// want to push a fix. So OTA gets its own task on the other core and is
// serviced every 10ms no matter what the main loop is stuck on.
void otaTask(void* param) {
  for (;;) {
    ArduinoOTA.handle();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}
#endif

void startOTA() {
#if ENABLE_OTA
  if (otaStarted) return;
  otaStarted = true;

  ArduinoOTA.setHostname("esp32-pinger");
  #ifdef OTA_PASSWORD
    ArduinoOTA.setPassword(OTA_PASSWORD);
  #endif

  ArduinoOTA.onStart([]() {
    otaInProgress = true;
    currentState  = OTA_ACTIVE;
    Serial.println("\n[*] OTA update starting...");
  });
  ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
    static uint8_t lastPct = 255;
    uint8_t pct = total ? (done * 100) / total : 0;
    if (pct != lastPct && pct % 10 == 0) {
      lastPct = pct;
      Serial.printf("[*] OTA %u%%\n", pct);
    }
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("[+] OTA complete, rebooting.");
    otaInProgress = false;
  });
  ArduinoOTA.onError([](ota_error_t err) {
    otaInProgress = false;
    Serial.printf("[-] OTA failed, error %u. Old firmware kept.\n", err);
  });

  // 1s (the default) is not enough slack for the first chunk to land.
  ArduinoOTA.setTimeout(15000);

  ArduinoOTA.begin();

  // core 0: the Arduino loop lives on core 1, so a blocked loop cannot stall this
  xTaskCreatePinnedToCore(otaTask, "otaTask", 8192, nullptr, 1, nullptr, 0);

  Serial.println("[+] OTA ready: esp32-pinger.local / " + WiFi.localIP().toString() + ":3232");
#endif
}

// Deferred until we have both a connection and a synced clock, so a mains
// outage that also took the router down still gets reported once it returns.
void doBootReportIfPending() {
  if (!bootReportPending) return;
  if (WiFi.status() != WL_CONNECTED) return;
  if (!clockOK()) { syncTime(4000); if (!clockOK()) return; }

  bootReportPending = false;
  sendBootReport();

  // A power loss is already covered by the boot report; only resume a
  // pending outage record when this was a soft reset.
  if (bootReason == ESP_RST_POWERON || bootReason == ESP_RST_BROWNOUT) {
    prefs.putULong("outSince", 0);
  } else if (pendingOutage > (time_t)MIN_EPOCH) {
    netDown        = true;
    netDownMsValid = false;
    netDownAt      = pendingOutage;
    netDownReason  = "device rebooted during the outage";
    outageCount++;
  }
}

// ================================================================ setup / loop

void setup() {
  Serial.begin(115200);
  delay(500);

  bootReason = esp_reset_reason();

  prefs.begin("pinger", false);
  bootCount      = prefs.getUInt("boots", 0) + 1;
  lastSeenAtBoot = (time_t)prefs.getULong("lastSeen", 0);
  pendingOutage  = (time_t)prefs.getULong("outSince", 0);
  prefs.putUInt("boots", bootCount);

  Serial.printf("\n[*] Boot #%lu, reason: %s\n",
                (unsigned long)bootCount, resetReasonStr(bootReason));

  for (uint8_t i = 0; i < SITE_COUNT; i++) {
    siteState[i] = { true, 0, 0, 0, 0, 0, 0, 0, 0 };
  }

  currentState = WIFI_DOWN;
  handleLEDs();

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("STA Failed to configure");
  }

  Serial.println("Initialising...");
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid, pw);
  Serial.println("Connecting...");

  // Don't block forever - loop() has proper reconnect handling.
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 30000) {
    handleLEDs();
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    syncTime(10000);

    startOTA();
    doBootReportIfPending();
    currentState = WIFI_CONNECTED_IDLE;
  } else {
    Serial.println("\n[-] Could not join WiFi in 30s, continuing in retry mode.");
  }

  handleLEDs();
  lastHeartbeatMs = millis();
}

void loop() {
  // While an image is streaming in, keep off the network and out of the heap.
  if (otaInProgress) {
    handleLEDs();
    delay(20);
    return;
  }

  handleWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    startOTA();
    doBootReportIfPending();
    flushAlertQueue();
#if ENABLE_TG_COMMANDS
    pollTelegram();
#endif
#if ENABLE_DAILY_DIGEST
    maybeDailyDigest();
#endif

    if (millis() - lastPingTime >= PING_INTERVAL || !firstCycleDone) {
      lastPingTime = millis();
      runCheckCycle();
    }
  }

  heartbeat();
  handleLEDs();
  delay(20);
}
