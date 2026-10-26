#include <Arduino.h>
/*
 * ESP32-S3 Server Pinger
 * ----------------------
 *  - Pings the LAN server + the internet every cycle
 *  - HTTPS health-checks a list of subdomains every cycle
 *  - Timestamps + durations for WiFi/internet outages
 *  - Detects power outages across reboots via an NVS heartbeat
 *  - Telegram alerts (queued while offline, flushed on recovery)
 *  - Telegram commands: /status /server /check /reboot
 *  - /server relays a status text the LAN server regenerates every minute
 *  - Relays iMac failover on/off reports (POST /failover, LAN) to Telegram
 *  - Planned reboot/shutdown signal from the server (POST /power): mutes the
 *    server-related alerts until it is back
 *  - Watches the server's own service-health-monitor through its heartbeat
 *    file, and says so here when it stops (it alerts through the server's
 *    bot, so if it dies nothing else would tell you)
 *  - Daily digest, OTA updates
 *  - Apple HomeKit (HomeSpan): two contact sensors, "Server" and "Websites"
 */

#include <WiFi.h>
#include <ESP32Ping.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <WiFiClientSecure.h>
#include <Preferences.h>
#include <esp_system.h>
#include <time.h>
#include "secrets.h"

// ---------------------------------------------------------------- features
#define ENABLE_OTA          1   // flash over WiFi instead of unplugging the board
#define ENABLE_TG_COMMANDS  1   // reply to /status /check /reboot in Telegram
#define ENABLE_DAILY_DIGEST 1
#define ENABLE_FAILOVER_API 1   // accept failover on/off reports from the iMac
#define ENABLE_POWER_API    1   // accept the server's planned reboot/shutdown signal
#define ENABLE_HOMEKIT      1   // two HomeKit contact sensors (needs HOMEKIT_SETUP_CODE in secrets.h)

#if ENABLE_OTA
  #include <ArduinoOTA.h>
#endif
#if ENABLE_HOMEKIT
  #include <HomeSpan.h>
  #include <sodium.h>   // HAP.h needs the crypto_* sizes declared first
  #include <HAP.h>      // HAPClient::nAdminControllers(): the only way to read the pairing state in 1.9.1
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

// Server details for /server. Plain HTTP on the LAN, by IP: this board resolves
// through public DNS on purpose (so the server being down cannot blind it), and
// the server's names only resolve to its LAN address through the server's own
// resolver. nginx answers this one path for LAN clients only.
const char*    SERVER_STATUS_URL        = "http://192.168.1.200/pinger/status.txt";
const uint16_t SERVER_STATUS_TIMEOUT_MS = 5000;
const uint16_t SERVER_STATUS_MAX_CHARS  = 1500;   // Telegram GET URL stays sane
// The server's service-health-monitor rewrites this every 15 s (same LAN vhost
// as status.txt). It holds key=value lines: updated=<epoch> state=ok|problems|
// stopped problems=<n> outbox=<n> tg_fail_since=<epoch or 0>. This board is the
// watcher of that watcher: it alerts here, through its own bot, because the
// monitor's alerts go through the server's sendtg, which cannot report its own
// death.
const char*    MONITOR_URL          = "http://192.168.1.200/pinger/health-monitor.txt";
const uint32_t MONITOR_STALE_SEC    = 150;    // heartbeat older than this = hung or dead
const uint8_t  MONITOR_FAIL_STRIKES = 2;      // bad cycles in a row before "DOWN"
const uint32_t MONITOR_GRACE_MS     = 300000; // after the server returns: let it boot
const uint32_t MONITOR_TG_FAIL_SEC  = 900;    // monitor alive but sendtg failing this long
// The server (wall-status.py) writes its combined verdict here every 5 s. Only
// level_server= (the verdict without this board's own reports) and ts= are read.
// An orange LED means the server reports a WARNING while this board sees green.
const char*    VERDICT_URL          = "http://192.168.1.200/pinger/verdict.txt";
const uint16_t VERDICT_TIMEOUT_MS   = 2000;      // short: loop() waits for it
const uint32_t VERDICT_POLL_MS      = 15000;     // idle reads at most this often
const uint32_t VERDICT_MAX_AGE_SEC  = 120;       // older than this = ignored
// The verdict also carries power=reboot|shutdown|none (the server's view of a
// planned power signal, see the mute below). A power=none only ends a mute when
// the file was written this long after the signal: wall-status needs a few
// seconds to notice the signal, and until then it still says none.
const uint32_t POWER_VERDICT_GUARD_SEC = 15;
const uint32_t POWER_MAX_SEC        = 86400;     // window/settle above this = bad input
// The iMac that stands in for the server on 443 (mhs-failover). It reports each
// takeover and hand-back with POST /failover?state=on|off; only it may.
// .254 is the shared 443 address, which it holds while failed over.
IPAddress failover_IP(192, 168, 1, 202);
IPAddress failover_VIP(192, 168, 1, 254);

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
const uint8_t   SITE_FAIL_STRIKES   = 3;      // consecutive fails before "DOWN" (2 tripped on ~10 s Cloudflare stalls every few days)
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
  OTA_ACTIVE,
  SERVER_REPORTS_WARNING
};
// Written by loop() and the OTA callback, read by the LED task on the other core.
volatile DeviceState currentState = WIFI_DOWN;

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
bool          firstCycleDone    = false;

// failover reports: written by the web task, acted on by loop()
volatile uint8_t failoverPending = 0;   // 0 none, 1 went on, 2 went off
volatile bool    failoverActive  = false;
volatile time_t  failoverSince   = 0;

// planned power signal (POST /power): written by the web task under stateMux,
// applied by loop() (handlePowerPending)
volatile uint8_t powerPending     = 0;   // 0 none, 1 reboot, 2 shutdown, 3 clear
time_t           powerPendingTs   = 0;   // server epoch of the signal (display only)
uint32_t         powerPendingWin  = 0;   // seconds, as received
uint32_t         powerPendingSet  = 0;
bool             powerPendingTest = false;

int      failedServerPings = 0;
bool     serverUp          = true;
time_t   serverDownAt      = 0;
uint32_t serverDownAtMs    = 0;
uint32_t serverBackAtMs    = 0;      // when pings resumed after an outage
time_t   lastServerOkAt    = 0;      // last good ping (epoch, 0 = none/unsynced)
bool     serverDownSentByPower = false;  // SERVER DOWN already sent at a reboot window's end
uint32_t cycleCount        = 0;      // check cycles started since boot

// Planned reboot/shutdown mute. Owned by loop(); timed with millis() only, as
// the board clock may be unsynced. Alerts of the muted kinds go through
// notifyAlert(), which drops them while powerMode != PM_NONE.
enum PowerMode : uint8_t { PM_NONE = 0, PM_REBOOT = 1, PM_SHUTDOWN = 2 };
uint8_t  powerMode        = PM_NONE;
time_t   powerTs          = 0;       // server epoch of the signal (display, verdict ts compare)
uint32_t powerStartMs     = 0;       // millis() when the POST was applied
uint32_t powerWindowMs    = 0;       // reboot: longest mute
uint32_t powerSettleMs    = 0;       // shutdown: server must have pinged this long
uint32_t powerUntil       = 0;       // epoch the reboot mute ends at the latest, 0 = unknown
bool     powerTest        = false;
uint32_t powerPingSinceMs = 0;       // millis() of the first good ping of the current run, 0 = not pinging
bool     powerDownKnown   = false;   // first outage already measured
uint32_t powerDownSec     = 0;       // first missed ping -> first good ping
bool     powerRechecked   = false;   // window ended with no ping: one fresh cycle was forced
uint32_t powerRecheckCycle = 0;

// the server's service-health-monitor, seen through its heartbeat
bool     monitorUp         = true;
uint8_t  monitorFails      = 0;
time_t   monitorDownAt     = 0;
uint32_t monitorDownAtMs   = 0;
String   monitorWhy        = "";
String   monitorLine       = "not checked yet";
bool     monitorTgAlerted  = false;

// What GET /state reports. Written by loop() on core 1, read by the web task
// on core 0: only plain integers, plus one char buffer copied under a
// spinlock -- never a String, which two cores must not share.
portMUX_TYPE stateMux = portMUX_INITIALIZER_UNLOCKED;
volatile uint32_t notifySeq      = 0;
volatile bool     lastNotifySent = false;
char              lastNotifyTitle[72] = "";

// What GET /state.json reports. loop() owns the plain variables and copies them
// into the pub* ones under stateMux (publishState); the web task only reads pub*.
enum VerdictLevel : uint8_t { VL_NONE = 0, VL_NORMAL = 1, VL_CAUTION = 2, VL_WARNING = 3 };
bool          lastInternetOK  = true;     // this board's own result in the last cycle
uint32_t      lastCycleTs     = 0;        // epoch when the last cycle ended, 0 = none
uint8_t       verdictLevel    = VL_NONE;  // last good level_server read
uint32_t      verdictAtMs     = 0;        // millis() of that read
bool          verdictRead     = false;    // any good read yet
long          verdictTs       = 0;        // ts= of that read (server clock)
enum VerdictPower : uint8_t { VP_UNKNOWN = 0, VP_NONE = 1, VP_ACTIVE = 2 };
uint8_t       verdictPower    = VP_UNKNOWN;  // power= of that read (VP_UNKNOWN = line absent)
uint32_t      lastVerdictTry  = 0;
String        verdictSuppressed = ",";  // suppressed= of that read, as ",a,b," (part H)

struct PubSite { bool up; int code; uint8_t fails; uint32_t downSince; };
PubSite           pubSite[SITE_COUNT];
bool              pubInternet     = true;
bool              pubServerUp     = true;
bool              pubMonitorUp    = true;
uint32_t          pubCycleTs      = 0;
uint8_t           pubVerdictLevel = VL_NONE;
uint32_t          pubVerdictAtMs  = 0;
bool              pubVerdictRead  = false;
uint8_t           pubPowerType    = PM_NONE;
uint32_t          pubPowerTs      = 0;
uint32_t          pubPowerUntil   = 0;
bool              pubPowerTest    = false;

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

// HomeKit. hkSitesOpen is written by loop() when it decides the LED colour
// (runCheckCycle); hkStarted/hkPaired are written by the HomeSpan task. Plain
// volatile bools: one writer each, read by other tasks (HomeSpan task, web task).
// The Server sensor needs no flag: it reads currentState directly (hkServerOpen()).
volatile bool hkSitesOpen = false;   // a site is down and not suppressed
volatile bool hkStarted   = false;   // HomeSpan was started
volatile bool hkPaired    = false;   // at least one admin controller is paired

// S3 built-in RGB LED
#ifndef RGB_BUILTIN
#define RGB_BUILTIN 48
#endif
uint32_t lastRGBWritten = 0xFFFFFFFF;
uint32_t lastRGBWriteMs = 0;
#define LED_TICK_MS     10     // LED task period
#define LED_REFRESH_MS  1000   // re-send the colour even if unchanged

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

// HH:MM:SS of an epoch (a server ts or a ping time), for the power messages.
String fmtHMS(time_t t) {
  if (t < (time_t)MIN_EPOCH) return "unknown";
  struct tm ti;
  localtime_r(&t, &ti);
  char buf[16];
  strftime(buf, sizeof(buf), "%H:%M:%S", &ti);
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

// ---- the server's verdict, as far as this board's LED is concerned
const char* verdictName(uint8_t lvl) {
  switch (lvl) {
    case VL_NORMAL:  return "normal";
    case VL_CAUTION: return "caution";
    case VL_WARNING: return "warning";
    default:         return "unknown";
  }
}

uint32_t verdictAgeSec() { return verdictRead ? (millis() - verdictAtMs) / 1000 : 0; }

// True when the last good read said "warning" and is recent enough to trust.
bool verdictIsWarning() {
  return verdictRead && verdictLevel == VL_WARNING && verdictAgeSec() <= VERDICT_MAX_AGE_SEC;
}

// True when the server's fresh verdict lists this site as suppressed by the user
// (part H). No fresh read = nothing is suppressed.
bool siteSuppressed(const char* name) {
  return verdictRead && verdictAgeSec() <= VERDICT_MAX_AGE_SEC &&
         verdictSuppressed.indexOf("," + String(name) + ",") >= 0;
}

// The state to show when this board itself sees everything fine.
DeviceState greenState() { return verdictIsWarning() ? SERVER_REPORTS_WARNING : SERVER_UP; }

// Switch only between green and orange; every other state is left alone.
void applyVerdictToLed() {
  DeviceState s = currentState;
  if (s == SERVER_UP && verdictIsWarning())                    currentState = SERVER_REPORTS_WARNING;
  else if (s == SERVER_REPORTS_WARNING && !verdictIsWarning()) currentState = SERVER_UP;
}

// The Server contact sensor is OPEN exactly while the LED shows the server's red
// (flashing red = SERVER_DEAD_INTERNET_UP). Yellow (missed ping) and orange (the
// server's own WARNING verdict) leave it closed. currentState is a volatile enum,
// already read by the LED task on the other core. The OTA overlay is not part of it.
bool hkServerOpen() { return currentState == SERVER_DEAD_INTERNET_UP; }

// ================================================================ LEDs
// The LED is driven by its own task on core 0 (see ledTask), never by loop().
// loop() runs on core 1 and can sit in a ping or HTTPS timeout for tens of
// seconds; when it drove the LED, a flash froze in whichever half it was in --
// out of rhythm, or dark for the whole stall if that half was "off".

void writeRGB(uint8_t r, uint8_t g, uint8_t b) {
  uint32_t v = ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
  // Unchanged colours are still re-sent every LED_REFRESH_MS, so a write the
  // LED missed cannot leave it dark or wrong until the next state change.
  if (v == lastRGBWritten && millis() - lastRGBWriteMs < LED_REFRESH_MS) return;
  lastRGBWritten = v;
  lastRGBWriteMs = millis();
  neopixelWrite(RGB_BUILTIN, r, g, b);
}

void setLED(DeviceState state, uint8_t r, uint8_t g, uint8_t b) {
  // Scale down heavily to absolute single digits because the hardware curve is clapped
  uint8_t dim_r = r > 0 ? 8 : 0;
  uint8_t dim_g = g > 0 ? 8 : 0;
  uint8_t dim_b = b > 0 ? 8 : 0;

  if (r == 128 && b == 128) { dim_r = 4; dim_b = 4; }   // purple
  if (r == 255 && g == 255) { dim_r = 4; dim_g = 4; }   // yellow
  if (r == 255 && g == 80)  { dim_r = 8; dim_g = 2; }   // orange (not yellow: green must stay low)

  if (state == SERVER_DEAD_INTERNET_UP) {
    writeRGB(r, g, b);          // full brightness, this one matters
  } else {
    writeRGB(dim_r, dim_g, dim_b);
  }
}

// The on/off phase comes from the clock, not from a toggle, so the rhythm is
// exact and cannot drift however late a tick runs.
void flash(DeviceState state, uint8_t r, uint8_t g, uint8_t b, uint16_t period) {
  if ((millis() / period) % 2 == 0) setLED(state, r, g, b);
  else                              setLED(state, 0, 0, 0);
}

void showState(DeviceState state) {
  switch (state) {
    case WIFI_DOWN:               setLED(state, 255, 0, 0);          break; // red
    case WIFI_CONNECTED_IDLE:     setLED(state, 128, 0, 128);        break; // purple
    case NO_INTERNET_WIFI_OK:     flash(state, 128, 0, 128, 500);    break; // purple flash
    case SERVER_UP:               setLED(state, 0, 255, 0);          break; // green
    case SERVER_MISSED_LATEST:    setLED(state, 255, 255, 0);        break; // yellow
    case SERVER_DEAD_INTERNET_UP: flash(state, 255, 0, 0, 500);      break; // red flash
    case SITE_DOWN_SERVER_UP:     flash(state, 0, 0, 255, 800);      break; // blue flash
    case OTA_ACTIVE:              flash(state, 0, 0, 255, 120);      break; // fast blue = flashing
    case SERVER_REPORTS_WARNING:  flash(state, 255, 80, 0, 800);     break; // orange flash = server reports a WARNING
  }
}

// Sole owner of the LED. Priority 2 keeps it ahead of the OTA and web tasks
// (priority 1) on core 0; it sleeps between ticks, so it costs next to nothing.
void ledTask(void* param) {
  for (;;) {
    // OTA wins even if a check cycle that was already running overwrites
    // currentState after the upload started.
    showState(otaInProgress ? OTA_ACTIVE : currentState);
    vTaskDelay(pdMS_TO_TICKS(LED_TICK_MS));
  }
}

void startLEDs() {
  xTaskCreatePinnedToCore(ledTask, "ledTask", 3072, nullptr, 2, nullptr, 0);
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

void recordNotify(const String &msg, bool sent) {
  int nl = msg.indexOf('\n');
  String title = (nl < 0) ? msg : msg.substring(0, nl);
  portENTER_CRITICAL(&stateMux);
  strlcpy(lastNotifyTitle, title.c_str(), sizeof(lastNotifyTitle));
  lastNotifySent = sent;
  notifySeq++;
  portEXIT_CRITICAL(&stateMux);
}

void notify(const String &msg) {
  Serial.println("---- TELEGRAM ----\n" + msg + "\n------------------");
  bool sent = sendTelegramNow(msg);
  recordNotify(msg, sent);
  if (sent) return;

  if (qCount == ALERT_QUEUE_SIZE) {          // drop the oldest
    qHead = (qHead + 1) % ALERT_QUEUE_SIZE;
    qCount--;
  }
  alertQueue[(qHead + qCount) % ALERT_QUEUE_SIZE] = msg;
  qCount++;
  Serial.printf("[*] Queued alert (%u pending).\n", qCount);
}

// Every alert of a kind the planned power signal mutes (SERVER DOWN/UP, SITE
// DOWN/UP, HEALTH MONITOR DOWN/UP, SERVER ALERTS BLOCKED/FLOWING AGAIN,
// FAILOVER ON/OFF) goes through here. While muted it is dropped, not queued.
// Everything else keeps using notify().
void notifyAlert(const String &msg) {
  if (powerMode != PM_NONE) {
    int nl = msg.indexOf('\n');
    Serial.println("[*] Muted (power signal): " + (nl < 0 ? msg : msg.substring(0, nl)));
    return;
  }
  notify(msg);
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

// The text of each muted alert kind that can be repeated when a mute ends
// while the thing is still down. Shared by the normal path and the mute's end.
String serverDownMsg(int misses) {
  String m  = "SERVER DOWN: mainhomeserver\n\n";
         m += String(misses) + " consecutive pings missed.\n";
         m += "First miss: " + fmtTime(serverDownAt) + "\n";
         m += "Confirmed:  " + fmtTime(nowEpoch());
  return m;
}

String siteDownMsg(uint8_t i) {
  const SiteState &st = siteState[i];
  String why = (st.lastCode > 0) ? ("HTTP " + String(st.lastCode))
                                 : "no response / connection refused / TLS failure";
  String m  = "SITE DOWN: " + String(SITES[i].name) + "\n\n";
         m += String(SITES[i].url) + "\n";
         m += "First failed: " + fmtTime(st.firstFailAt) + "\n";
         m += "Confirmed:    " + fmtTime(nowEpoch()) + " after " +
              String(st.fails) + " tries\n";
         m += "Reason: " + why;
  return m;
}

String monitorDownMsg() {
  return "HEALTH MONITOR DOWN: service-health-monitor\n\n"
         "The server answers pings, but its service monitor is not working: " + monitorWhy + "\n\n"
         "First failed: " + fmtTime(monitorDownAt) + "\n"
         "Confirmed:    " + fmtTime(nowEpoch()) + " after " + String(monitorFails) + " checks\n\n"
         "Until it is back, nothing tells you when a service on the server dies or slows down.\n"
         "On the server: systemctl status service-health-monitor";
}

String failoverOnMsg() {
  return "FAILOVER ON: the iMac is answering 443\n\n"
         "Since: " + fmtTime(failoverSince) + "\n"
         "mainhomeserver failed its web check and gave up 192.168.1.254, so visitors\n"
         "to the grey-clouded sites now get the \"temporarily down\" page from the iMac.\n"
         "dllmch.org, www, tools and mc get it from the Cloudflare Worker.";
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
  if (failoverActive) s += "\nFailover: iMac is answering 443 since " + fmtTime(failoverSince);
  s += "\nHealth monitor: " + String(monitorUp ? "" : "DOWN since " + fmtTime(monitorDownAt) + " -- ") + monitorLine;
  s += "\nServer verdict: " + (verdictRead ? String(verdictName(verdictLevel)) + " (" + String(verdictAgeSec()) + " s ago)"
                                           : String("unknown (never read)"));
  if (powerMode != PM_NONE)
    s += String("\nPower signal: ") + (powerMode == PM_REBOOT ? "reboot" : "shutdown") + " at " +
         fmtHMS(powerTs) + ", server alerts muted";
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

// Fetch the server's own status text. Returns a message either way, so the
// caller can hand it straight to Telegram.
String fetchServerStatus() {
  if (WiFi.status() != WL_CONNECTED) return "Cannot reach the server: WiFi is down.";

  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(SERVER_STATUS_TIMEOUT_MS);
  http.setTimeout(SERVER_STATUS_TIMEOUT_MS);
  http.setUserAgent("ESP32-Pinger/2.0");

  uint32_t t0 = millis();
  if (!http.begin(client, SERVER_STATUS_URL)) return "Cannot reach the server: HTTP client failed to start.";
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  uint32_t ms = millis() - t0;
  Serial.printf("[%s] server status -> code %d (%lu ms)\n", code == 200 ? "+" : "-", code, (unsigned long)ms);

  if (code != 200) {
    String why = (code > 0) ? ("HTTP " + String(code)) : "no response / connection refused";
    return "SERVER DETAILS unavailable (" + why + " after " + String(ms) + " ms)\n\n"
           "The LAN server (" + server_IP.toString() + ") is " + String(serverUp ? "answering pings" : "NOT answering pings") +
           ". If it is up, check the server-status timer and the nginx LAN vhost on it.";
  }
  body.trim();
  if (body.length() > SERVER_STATUS_MAX_CHARS) body = body.substring(0, SERVER_STATUS_MAX_CHARS) + "\n[...]";
  return "SERVER DETAILS (fetched in " + String(ms) + " ms)\n\n" + body;
}

// ================================================================ checks

// One "key=value" line from the heartbeat body, or "" if absent.
String hbField(const String &body, const char* key) {
  String k = String(key) + "=";
  int i = body.startsWith(k) ? 0 : body.indexOf("\n" + k);
  if (i < 0) return "";
  if (i > 0) i++;                       // skip the newline
  int e = body.indexOf('\n', i);
  return body.substring(i + k.length(), e < 0 ? body.length() : e);
}

// Returns true if the heartbeat looks healthy; sets why (for alerts) and
// monitorLine (for /status) either way.
bool fetchMonitorHeartbeat(String &why) {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(SERVER_STATUS_TIMEOUT_MS);
  http.setTimeout(SERVER_STATUS_TIMEOUT_MS);
  http.setUserAgent("ESP32-Pinger/2.0");
  if (!http.begin(client, MONITOR_URL)) { why = "HTTP client failed to start"; monitorLine = why; return false; }
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();

  if (code != 200) {
    why = "its heartbeat page does not answer (" +
          String(code > 0 ? "HTTP " + String(code) : String("no response")) +
          "). Either the monitor never wrote it, or nginx on the server is down.";
    monitorLine = "heartbeat unreadable (" + String(code) + ")";
    return false;
  }
  String state   = hbField(body, "state");
  long   updated = hbField(body, "updated").toInt();
  long   probs   = hbField(body, "problems").toInt();
  long   tgFail  = hbField(body, "tg_fail_since").toInt();
  long   outbox  = hbField(body, "outbox").toInt();

  // The alert path of the server: the monitor is alive but sendtg fails.
  if (clockOK() && tgFail > 0 && nowEpoch() - tgFail > (long)MONITOR_TG_FAIL_SEC) {
    if (!monitorTgAlerted) {
      monitorTgAlerted = true;
      notifyAlert("SERVER ALERTS BLOCKED\n\n"
             "service-health-monitor on mainhomeserver runs, but it cannot send its alerts "
             "through sendtg (the server's Telegram bot) since " + fmtTime((time_t)tgFail) + ".\n"
             "Alerts waiting: " + String(outbox) + "\n\n"
             "Problems it sees now: " + String(probs) + ". Ask /server for details.");
    }
  } else if (monitorTgAlerted && tgFail == 0) {
    monitorTgAlerted = false;
    notifyAlert("SERVER ALERTS FLOWING AGAIN\n\nservice-health-monitor can send through sendtg again; "
           "the alerts it held are delivered now.");
  }

  if (state == "stopped") {
    why = "it was stopped (state=stopped: a clean exit, e.g. systemctl stop), and nothing started it again.";
    monitorLine = "STOPPED";
    return false;
  }
  if (clockOK() && updated > (long)MIN_EPOCH) {
    long age = (long)nowEpoch() - updated;
    if (age > (long)MONITOR_STALE_SEC) {
      why = "its heartbeat is " + fmtDur((uint32_t)age) + " old (last beat " + fmtTime((time_t)updated) +
            "). It hung or crashed, and systemd did not bring it back.";
      monitorLine = "heartbeat " + fmtDur((uint32_t)age) + " old";
      return false;
    }
    monitorLine = String(state == "ok" ? "OK" : "running") + ", beat " + String(age < 0 ? 0 : age) +
                  "s ago, " + String(probs) + " open problem" + String(probs == 1 ? "" : "s");
  } else {
    monitorLine = "running (clock not synced, age unknown)";
  }
  return true;
}

void checkMonitor() {
  // The server being down is already its own alert; and right after it comes
  // back, give the boot time to start the monitor before judging it.
  if (!serverUp || failedServerPings > 0) return;
  if (serverBackAtMs && millis() - serverBackAtMs < MONITOR_GRACE_MS) return;

  String why;
  bool ok = fetchMonitorHeartbeat(why);
  Serial.printf("[%s] health monitor -> %s\n", ok ? "+" : "-", monitorLine.c_str());

  if (ok) {
    if (!monitorUp) {
      monitorUp = true;
      uint32_t dur = monitorDownAtMs ? (millis() - monitorDownAtMs) / 1000 : 0;
      notifyAlert("HEALTH MONITOR UP: service-health-monitor\n\n"
             "Recovered: " + fmtTime(nowEpoch()) + "\n"
             "Went down: " + fmtTime(monitorDownAt) + "\n"
             "Down for:  " + fmtDur(dur) + "\n"
             "Now: " + monitorLine + ".\n\n"
             "It sends its own alerts again. Anything that broke while it was down is "
             "reported by it now only if it is still broken.");
    }
    monitorFails = 0;
    return;
  }
  monitorFails++;
  if (monitorFails == 1) { monitorDownAt = nowEpoch(); monitorDownAtMs = millis(); }
  if (monitorUp && monitorFails >= MONITOR_FAIL_STRIKES) {
    monitorUp  = false;
    monitorWhy = why;
    notifyAlert(monitorDownMsg());
  }
}

// One good read = HTTP 200 and a level_server this board knows. Anything else
// leaves the last good read untouched and changes nothing else.
bool fetchVerdict() {
  WiFiClient client;
  HTTPClient http;
  http.setConnectTimeout(VERDICT_TIMEOUT_MS);
  http.setTimeout(VERDICT_TIMEOUT_MS);
  http.setUserAgent("ESP32-Pinger/2.0");
  if (!http.begin(client, VERDICT_URL)) return false;
  int code = http.GET();
  String body = (code == 200) ? http.getString() : "";
  http.end();
  if (code != 200) {
    Serial.printf("[-] server verdict -> code %d\n", code);
    return false;
  }

  String lvl = hbField(body, "level_server");
  lvl.trim();
  uint8_t level = VL_NONE;
  if      (lvl == "normal")  level = VL_NORMAL;
  else if (lvl == "caution") level = VL_CAUTION;
  else if (lvl == "warning") level = VL_WARNING;
  if (level == VL_NONE) {
    Serial.println("[-] server verdict -> no valid level_server");
    return false;
  }
  // A file that stopped updating (wall-status dead) is not a verdict.
  long ts = hbField(body, "ts").toInt();
  if (clockOK() && (ts <= 0 || (long)nowEpoch() - ts > (long)VERDICT_MAX_AGE_SEC)) {
    Serial.println("[-] server verdict -> stale ts");
    return false;
  }
  // power= (part B of the contract): only "none" can end a mute; a file without
  // the line (older server) is "unknown" and never confirms anything.
  String pw = hbField(body, "power");
  pw.trim();
  verdictPower = (pw == "none") ? VP_NONE
               : (pw == "reboot" || pw == "shutdown") ? VP_ACTIVE : VP_UNKNOWN;
  // suppressed= (part H): a missing line reads as an empty list
  String sup = hbField(body, "suppressed");
  sup.trim();
  verdictSuppressed = "," + sup + ",";
  verdictLevel = level;
  verdictTs    = ts;
  verdictAtMs  = millis();
  verdictRead  = true;
  Serial.printf("[+] server verdict -> %s\n", verdictName(level));
  return true;
}

// Copy what the web task may show into the pub* variables. Called from loop()
// only, and only copies plain values inside the critical section.
void publishState() {
  portENTER_CRITICAL(&stateMux);
  pubInternet     = lastInternetOK;
  pubServerUp     = serverUp;
  pubMonitorUp    = monitorUp;
  pubCycleTs      = lastCycleTs;
  pubVerdictLevel = verdictLevel;
  pubVerdictAtMs  = verdictAtMs;
  pubVerdictRead  = verdictRead;
  pubPowerType    = powerMode;
  pubPowerTs      = (uint32_t)powerTs;
  pubPowerUntil   = powerUntil;
  pubPowerTest    = powerTest;
  for (uint8_t i = 0; i < SITE_COUNT; i++) {
    const SiteState &st = siteState[i];
    pubSite[i].up    = st.up;
    pubSite[i].code  = st.lastCode;
    pubSite[i].fails = st.fails;
    pubSite[i].downSince = (!st.up && st.firstFailAt > (time_t)MIN_EPOCH) ? (uint32_t)st.firstFailAt : 0;
  }
  portEXIT_CRITICAL(&stateMux);
}

// Read the verdict when it is due (force = at the end of a cycle). Skipped while
// the server looks down or WiFi/OTA is busy. A failed read never raises an alert.
void readVerdict(bool force) {
  if (otaInProgress || WiFi.status() != WL_CONNECTED) return;
  if (!serverUp || failedServerPings > 0) return;
  if (!force && millis() - lastVerdictTry < VERDICT_POLL_MS) return;
  lastVerdictTry = millis();
  fetchVerdict();
  applyVerdictToLed();
  publishState();
}

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
       " + its service-health-monitor, every " + String(PING_INTERVAL / 1000) + "s.";
  notify(m);
}

// ================================================================ Telegram commands

#if ENABLE_TG_COMMANDS
long tgOffset = 0;

// Telegram keeps handing back an update until getUpdates is called with an
// offset past it; an unconfirmed one sits in the queue for ~24h. That makes a
// reboot command self-repeating: reboot, poll, get the same /reboot, reboot.
// The offset therefore has to survive the restart (NVS) *and* be confirmed
// with Telegram before the board goes down (tgConfirmOffset).
void saveTgOffset() {
  prefs.putULong("tgOffset", (uint32_t)tgOffset);
}

void tgConfirmOffset() {
  if (!tgOffset || WiFi.status() != WL_CONNECTED) return;

  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient http;
  http.setConnectTimeout(6000);
  http.setTimeout(8000);

  String url = "https://api.telegram.org/bot" + tg_token +
               "/getUpdates?timeout=0&limit=1&offset=" + String(tgOffset);
  if (!http.begin(client, url)) return;
  http.GET();
  http.end();
}

String helpText() {
  return
    "Server Pinger - available commands\n"
    "\n"
    "/status\n"
    "    Current state: uptime, boot count, outage totals, per-site\n"
    "    availability, WiFi signal and free heap.\n"
    "\n"
    "/server\n"
    "    Details from the server itself: uptime, load, temperature,\n"
    "    memory, disks, RAID, containers, failed services.\n"
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
    "or recovers, when the internet drops, after a power cut, when\n"
    "the iMac takes over or hands back port 443, and when the server's\n"
    "service-health-monitor stops or cannot send its own alerts.";
}

void handleCommand(const String &cmd) {
  Serial.println("[*] Command: " + cmd);

  if (cmd.startsWith("/status") || cmd.startsWith("/uptime")) {
    notify("STATUS\n\n" + buildStatus());
  } else if (cmd.startsWith("/server")) {
    notify(fetchServerStatus());
  } else if (cmd.startsWith("/check")) {
    notify("Running a check cycle now...");
    lastPingTime = millis() - PING_INTERVAL; // force the next loop() to run one
  } else if (cmd.startsWith("/reboot")) {
    notify("Rebooting on request.");
    tgConfirmOffset();   // or Telegram replays this command into the next boot
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
    if (id >= tgOffset) { tgOffset = id + 1; saveTgOffset(); }

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

// ================================================================ planned power signal

// The server announces a planned reboot or shutdown with POST /power (see the
// handler below). From then on the muted alert kinds are dropped (notifyAlert)
// until one of the end conditions below holds. Everything else -- checks, state,
// LED, state.json, internet/WiFi messages -- runs exactly as without a signal.
// All timing is millis() from the moment the POST was applied: the board clock
// can be unsynced, and a server ts is only ever displayed.

String powerName(uint8_t m) { return m == PM_REBOOT ? "reboot" : "shutdown"; }

// "0s" when no ping was missed, else a normal duration.
String fmtDown(uint32_t sec) { return sec == 0 ? String("0s") : fmtDur(sec); }

// Called by the check cycle with every server ping result, before the strike
// counters change. Measures the first outage (first missed ping -> first good
// ping) and the unbroken run of good pings that ends a shutdown mute.
void powerOnPing(bool ok) {
  if (powerMode == PM_NONE) return;
  if (ok) {
    if (failedServerPings > 0 && !powerDownKnown) {
      powerDownKnown = true;
      powerDownSec   = serverDownAtMs ? (millis() - serverDownAtMs) / 1000 : 0;
    }
    if (!powerPingSinceMs) powerPingSinceMs = millis() | 1;   // |1: never 0 = "not pinging"
  } else {
    powerPingSinceMs = 0;
  }
}

// The mute is over. back = the server answers pings now.
void powerEnd(bool back) {
  uint8_t  mode = powerMode;
  bool     test = powerTest;
  time_t   ts   = powerTs;
  uint32_t down = powerDownKnown ? powerDownSec : 0;
  powerMode = PM_NONE;
  powerUntil = 0;
  publishState();

  if (back) {
    notify(String(test ? "TEST " : "") + "SERVER BACK after " + powerName(mode) + "\n\n"
           "Signal: " + fmtHMS(ts) + "\n"
           "Down for: " + fmtDown(down));
    // what is down or on NOW; what went down and recovered inside the mute stays silent
    for (uint8_t i = 0; i < SITE_COUNT; i++)
      if (!siteState[i].up) notify(siteDownMsg(i));
    if (!monitorUp) notify(monitorDownMsg());
    if (failoverActive) notify(failoverOnMsg());
  } else {
    // reboot window over, no ping: the normal SERVER DOWN, now. The strike logic
    // must not send it a second time (serverDownSentByPower), and a later
    // recovery must send SERVER UP even if it came before the third strike.
    notify(serverDownMsg(failedServerPings) + "\n\n"
           "Did not come back after its reboot signal. Last seen " + fmtHMS(lastServerOkAt) +
           " with reboot signal.");
    if (failedServerPings < SERVER_FAIL_STRIKES) serverDownSentByPower = true;
  }
}

// Checked every loop() pass while WiFi is up.
void powerTick() {
  if (powerMode == PM_NONE) return;
  bool pinging = serverUp && failedServerPings == 0;

  // the server's own word: a fresh read, written well after the signal, says none
  bool verdictNone = pinging && verdictRead && verdictPower == VP_NONE &&
                     verdictAgeSec() <= VERDICT_MAX_AGE_SEC &&
                     verdictTs >= (long)powerTs + (long)POWER_VERDICT_GUARD_SEC;
  if (verdictNone) { powerEnd(true); return; }

  if (powerMode == PM_SHUTDOWN) {
    // no limit while the server does not ping
    if (pinging && powerPingSinceMs && millis() - powerPingSinceMs >= powerSettleMs) powerEnd(true);
    return;
  }

  if (millis() - powerStartMs < powerWindowMs) return;
  if (pinging) { powerEnd(true); return; }
  // Window over and the last cycle saw no ping (up to a minute old): look once
  // more before saying so.
  if (!powerRechecked) {
    powerRechecked    = true;
    powerRecheckCycle = cycleCount;
    lastPingTime      = millis() - PING_INTERVAL;     // the next loop() pass runs a cycle
    return;
  }
  if (cycleCount == powerRecheckCycle) return;        // that cycle has not run yet
  powerEnd(false);
}

#if ENABLE_POWER_API
// Apply what the web task stored: start, replace or clear the mute, and send the
// announcement (not muted itself).
void handlePowerPending() {
  uint8_t  p;
  time_t   ts;
  uint32_t win, settle;
  bool     test;
  portENTER_CRITICAL(&stateMux);
  p = powerPending;  ts = powerPendingTs;  win = powerPendingWin;
  settle = powerPendingSet;  test = powerPendingTest;
  powerPending = 0;
  portEXIT_CRITICAL(&stateMux);
  if (!p) return;

  if (p == 3) {                                       // clear: end at once, say nothing
    powerMode = PM_NONE;
    powerUntil = 0;
    publishState();
    Serial.println("[*] Power signal cleared.");
    return;
  }

  powerMode        = p;                               // a new signal replaces the old one
  powerTs          = ts;
  powerTest        = test;
  powerStartMs     = millis();
  powerWindowMs    = win * 1000UL;
  powerSettleMs    = settle * 1000UL;
  powerUntil       = (p == PM_REBOOT && clockOK()) ? (uint32_t)(ts + (time_t)win) : 0;
  powerPingSinceMs = (serverUp && failedServerPings == 0) ? (millis() | 1) : 0;
  powerDownKnown   = false;
  powerDownSec     = 0;
  powerRechecked   = false;
  publishState();
  Serial.printf("[*] Power signal: %s, window %lu s, settle %lu s%s\n", powerName(p).c_str(),
                (unsigned long)win, (unsigned long)settle, test ? " (test)" : "");

  String head = test ? "TEST " : "";
  if (p == PM_REBOOT) {
    String mins = (win % 60 == 0) ? String(win / 60) + " min" : String(win) + " s";
    notify(head + "SERVER REBOOTING: mainhomeserver\n\n"
           "Signal: " + fmtHMS(ts) + "\n"
           "Alerts muted until " + fmtHMS(ts + (time_t)win) + " at the latest (" + mins +
           "). If it is not back by then, you get SERVER DOWN.");
  } else {
    notify(head + "SERVER SHUTTING DOWN: mainhomeserver\n\n"
           "Signal: " + fmtHMS(ts) + "\n"
           "Alerts muted until it is back and settled.");
  }
}
#endif  // ENABLE_POWER_API

// ================================================================ check cycle

void checkSites() {
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
        notifyAlert(m);
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
        notifyAlert(siteDownMsg(i));
      }
    }

    publishState();
    delay(50);                                 // let the TLS context tear down
  }
}

// A site that is down and not suppressed by the user (part H). Uses the
// verdict, so call it after readVerdict().
bool anySiteDownUnsuppressed() {
  for (uint8_t i = 0; i < SITE_COUNT; i++)
    if (!siteState[i].up && !siteSuppressed(SITES[i].name)) return true;
  return false;
}

void runCheckCycle() {
  Serial.println("\n[*] ===== check cycle =====");
  cycleCount++;

  Serial.println("[*] Pinging Internet...");
  if (!checkInternet()) {
    lastInternetOK = false;
    beginNetOutage("WiFi associated but 1.1.1.1 and 8.8.8.8 both unreachable");
    currentState = NO_INTERNET_WIFI_OK;
    if (clockOK()) lastCycleTs = (uint32_t)nowEpoch();
    publishState();
    return;
  }
  lastInternetOK = true;
  endNetOutage();                              // sends the recovery message if needed

  if (!clockOK()) syncTime(5000);

  // ---- LAN server ----
  Serial.println("[*] Pinging Server...");
  if (Ping.ping(server_IP, 1)) {
    if (failedServerPings >= SERVER_FAIL_STRIKES || serverDownSentByPower) {
      serverBackAtMs = millis();
      uint32_t dur = serverDownAtMs ? (millis() - serverDownAtMs) / 1000 : 0;
      String m  = "SERVER UP: mainhomeserver\n\n";
             m += "Recovered: " + fmtTime(nowEpoch()) + "\n";
             m += "Went down: " + fmtTime(serverDownAt) + "\n";
             m += "Down for:  " + fmtDur(dur);
      notifyAlert(m);
    }
    powerOnPing(true);
    failedServerPings = 0;
    serverDownSentByPower = false;
    lastServerOkAt = clockOK() ? nowEpoch() : 0;
    serverUp = true;
    // Stay blue while the sites are rechecked below, rather than showing green
    // for the tens of seconds that takes; the end of the cycle settles it.
    if (currentState != SITE_DOWN_SERVER_UP) currentState = greenState();
    Serial.println("[+] Server is alive.");
  } else {
    powerOnPing(false);
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
        // a reboot window that ended with the server silent already said so
        if (!serverDownSentByPower) notifyAlert(serverDownMsg(SERVER_FAIL_STRIKES));
      }
    }
  }

  // ---- the server's own service monitor ----
  checkMonitor();

  // ---- public sites ----
  if (SITE_COUNT) {
    Serial.println("[*] Checking sites...");
    checkSites();
  }

  // the server's verdict, once per cycle (idle reads in loop() keep it fresh)
  readVerdict(true);

  // HomeKit "Websites" sensor: exactly the blue LED condition (down, not
  // suppressed, and only while the LAN server looks fine), so a dead server
  // opens "Server" alone (user's choice, 2026-10-26)
  hkSitesOpen = serverUp && failedServerPings == 0 && anySiteDownUnsuppressed();

  // site trouble only colours the LED when the LAN server itself looks fine
  if (serverUp && failedServerPings == 0)
    currentState = anySiteDownUnsuppressed() ? SITE_DOWN_SERVER_UP : greenState();

  firstCycleDone = true;
  if (clockOK()) lastCycleTs = (uint32_t)nowEpoch();
  publishState();
  Serial.println("[*] ===== cycle done =====\n");
}

// ================================================================ failover reports

#if ENABLE_FAILOVER_API || ENABLE_POWER_API
WebServer web(80);
#endif

#if ENABLE_FAILOVER_API
// The iMac (mhs-failover) posts here when it takes 192.168.1.254 -- the router's
// 443 target -- and when it gives it back. Like OTA, this is served from its
// own task on core 0, because loop() can sit in check timeouts for a minute.
// The handler only records the change; loop() sends the Telegram message, so
// two TLS clients never run at once.

void handleFailover() {
  IPAddress from = web.client().remoteIP();
  if (from != failover_IP && from != failover_VIP) {
    web.send(403, "text/plain", "forbidden\n");
    return;
  }
  if (web.method() != HTTP_POST) {
    web.send(405, "text/plain", "POST only\n");
    return;
  }
  String st = web.arg("state");
  if (st == "on") {
    if (!failoverActive) { failoverActive = true; failoverSince = nowEpoch(); failoverPending = 1; }
  } else if (st == "off") {
    if (failoverActive)  { failoverActive = false; failoverPending = 2; }
  } else {
    web.send(400, "text/plain", "state must be on or off\n");
    return;
  }
  web.send(200, "text/plain", "ok\n");
}
#endif  // ENABLE_FAILOVER_API

#if ENABLE_POWER_API
// Parse a positive decimal integer of at most 10 digits (no sign, no spaces).
static bool parsePosInt(const String &in, uint32_t &out) {
  if (in.length() == 0 || in.length() > 10) return false;
  uint64_t v = 0;
  for (size_t i = 0; i < in.length(); i++) {
    if (!isdigit((uint8_t)in[i])) return false;
    v = v * 10 + (in[i] - '0');
  }
  if (v == 0 || v > 0xFFFFFFFFULL) return false;
  out = (uint32_t)v;
  return true;
}

// POST /power?type=reboot|shutdown|clear&ts=&window=&settle=[&test=1], only from
// the server (192.168.1.200). Same split as /failover: this only records the
// request (under stateMux, flag last); loop() starts the mute and sends the
// Telegram message (handlePowerPending). Contract part F.
void handlePower() {
  IPAddress from = web.client().remoteIP();
  if (from != server_IP) {
    web.send(403, "text/plain", "forbidden\n");
    return;
  }
  if (web.method() != HTTP_POST) {
    web.send(405, "text/plain", "POST only\n");
    return;
  }
  String type = web.arg("type");
  uint8_t  code = 0;
  uint32_t ts = 0, win = 360, settle = 300;
  bool test = (web.arg("test") == "1");
  if (type == "clear") {
    code = 3;
  } else if (type == "reboot" || type == "shutdown") {
    code = (type == "reboot") ? PM_REBOOT : PM_SHUTDOWN;
    // ts is always needed; the value the type does not use may be left out
    bool ok = parsePosInt(web.arg("ts"), ts);
    if (web.hasArg("window") || code == PM_REBOOT) ok = ok && parsePosInt(web.arg("window"), win) && win <= POWER_MAX_SEC;
    if (web.hasArg("settle") || code == PM_SHUTDOWN) ok = ok && parsePosInt(web.arg("settle"), settle) && settle <= POWER_MAX_SEC;
    if (!ok) {
      web.send(400, "text/plain", "ts, window (reboot) and settle (shutdown) must be positive integers\n");
      return;
    }
  } else {
    web.send(400, "text/plain", "type must be reboot, shutdown or clear\n");
    return;
  }

  portENTER_CRITICAL(&stateMux);
  powerPendingTs   = (time_t)ts;
  powerPendingWin  = win;
  powerPendingSet  = settle;
  powerPendingTest = test;
  powerPending     = code;
  portEXIT_CRITICAL(&stateMux);

  uint32_t until = (code == PM_REBOOT && clockOK()) ? ts + win : 0;
  String j = "{\"ok\":true,\"type\":\"" + type + "\",\"until\":" + String((unsigned long)until) +
             ",\"test\":" + (test ? "true" : "false") + "}";
  web.send(200, "application/json", j);
}
#endif  // ENABLE_POWER_API

#if ENABLE_FAILOVER_API || ENABLE_POWER_API
// Read-only state for the LAN, so the server side can test this board's
// monitor watch end to end (did it notice, and did Telegram take the alert?)
// without a USB cable. No secrets in it.
void handleState() {
  IPAddress from = web.client().remoteIP();
  if (from[0] != 192 || from[1] != 168 || from[2] != 1) {
    web.send(403, "text/plain", "forbidden\n");
    return;
  }
  char title[sizeof(lastNotifyTitle)];
  portENTER_CRITICAL(&stateMux);
  memcpy(title, lastNotifyTitle, sizeof(title));
  uint32_t seq = notifySeq;
  bool sent = lastNotifySent;
  uint8_t pw = pubPowerType;
  portEXIT_CRITICAL(&stateMux);
  char buf[460];
  snprintf(buf, sizeof(buf),
           "uptime_s=%lu\nserver_up=%d\nmonitor_up=%d\nmonitor_fails=%u\n"
           "notify_seq=%lu\nlast_notify=%s\nlast_notify_sent=%d\nqueued=%u\npower=%s\n"
           "homekit_started=%d\nhomekit_paired=%d\nhomekit_server_open=%d\nhomekit_sites_open=%d\n",
           (unsigned long)(millis() / 1000), (int)serverUp, (int)monitorUp, (unsigned)monitorFails,
           (unsigned long)seq, title, (int)sent, (unsigned)qCount,
           pw == PM_REBOOT ? "reboot" : pw == PM_SHUTDOWN ? "shutdown" : "none",
           (int)(ENABLE_HOMEKIT && hkStarted), (int)(ENABLE_HOMEKIT && hkStarted && hkPaired),
           (int)hkServerOpen(), (int)hkSitesOpen);
  web.send(200, "text/plain", buf);
}

// Append s to out as the inside of a JSON string: quote and backslash escaped,
// control characters dropped.
static void jsonEscapeAppend(String &out, const char* s) {
  for (; *s; s++) {
    char c = *s;
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c >= 0x20) out += c;
  }
}

static const char* ledName(DeviceState st, bool ota) {
  if (ota) return "ota";
  switch (st) {
    case WIFI_DOWN:               return "wifi_down";
    case WIFI_CONNECTED_IDLE:     return "wifi_idle";
    case NO_INTERNET_WIFI_OK:     return "no_internet";
    case SERVER_UP:               return "server_up";
    case SERVER_MISSED_LATEST:    return "server_missed";
    case SERVER_DEAD_INTERNET_UP: return "server_dead";
    case SITE_DOWN_SERVER_UP:     return "site_down";
    case OTA_ACTIVE:              return "ota";
    case SERVER_REPORTS_WARNING:  return "server_warning";
  }
  return "wifi_down";
}

// Machine-readable state for the wall page and the server (interface contract
// 2026-10-16, part A). LAN only. Everything loop() writes comes from the pub*
// copies, read under stateMux; the JSON is built afterwards on the heap.
void handleStateJson() {
  web.sendHeader("Access-Control-Allow-Origin", "*");
  web.sendHeader("Cache-Control", "no-store");
  IPAddress from = web.client().remoteIP();
  if (from[0] != 192 || from[1] != 168 || from[2] != 1) {
    web.send(403, "text/plain", "forbidden\n");
    return;
  }

  PubSite  sites[SITE_COUNT];
  bool     internet, srvUp, monUp, vRead;
  uint32_t cycleTs, vAt, nowMs;
  uint8_t  vLevel, pwType;
  uint32_t pwTs, pwUntil;
  bool     pwTest;
  portENTER_CRITICAL(&stateMux);
  memcpy(sites, pubSite, sizeof(sites));
  internet = pubInternet;  srvUp = pubServerUp;  monUp = pubMonitorUp;
  cycleTs  = pubCycleTs;
  vLevel   = pubVerdictLevel;  vAt = pubVerdictAtMs;  vRead = pubVerdictRead;
  pwType   = pubPowerType;  pwTs = pubPowerTs;  pwUntil = pubPowerUntil;  pwTest = pubPowerTest;
  nowMs    = millis();
  portEXIT_CRITICAL(&stateMux);

  long vAge = vRead ? (long)((nowMs - vAt) / 1000) : -1;
  const char* vName = (vRead && vAge <= (long)VERDICT_MAX_AGE_SEC) ? verdictName(vLevel) : "unknown";

  String j;
  j.reserve(1600);
  j += "{\"v\":1,\"uptime_s\":";   j += String((unsigned long)(nowMs / 1000));
  j += ",\"now\":";                  j += String((unsigned long)(clockOK() ? nowEpoch() : 0));
  j += ",\"wifi_rssi\":";            j += String((long)WiFi.RSSI());
  j += ",\"internet\":";             j += internet ? "true" : "false";
  j += ",\"server_up\":";            j += srvUp ? "true" : "false";
  j += ",\"monitor_up\":";           j += monUp ? "true" : "false";
  j += ",\"led\":\"";                j += ledName(currentState, otaInProgress);
  j += "\",\"cycle_ts\":";           j += String((unsigned long)cycleTs);
  j += ",\"verdict_level\":\"";      j += vName;
  j += "\",\"verdict_age_s\":";      j += String(vAge);
  if (pwType == PM_NONE) {
    j += ",\"power\":null";
  } else {
    j += ",\"power\":{\"type\":\"";   j += pwType == PM_REBOOT ? "reboot" : "shutdown";
    j += "\",\"ts\":";                j += String((unsigned long)pwTs);
    j += ",\"until\":";               j += String((unsigned long)pwUntil);
    j += ",\"test\":";                j += pwTest ? "true" : "false";
    j += '}';
  }
  j += ",\"sites\":[";
  for (uint8_t i = 0; i < SITE_COUNT; i++) {
    if (i) j += ',';
    j += "{\"name\":\"";             jsonEscapeAppend(j, SITES[i].name);
    j += "\",\"up\":";               j += sites[i].up ? "true" : "false";
    j += ",\"code\":";               j += String((long)sites[i].code);
    j += ",\"fails\":";              j += String((long)sites[i].fails);
    j += ",\"down_since\":";         j += String((unsigned long)sites[i].downSince);
    j += '}';
  }
  j += "],\"homekit\":{\"enabled\":";  j += ENABLE_HOMEKIT ? "true" : "false";
  j += ",\"started\":";                  j += hkStarted ? "true" : "false";
  j += ",\"paired\":";                   j += !hkStarted ? "null" : hkPaired ? "true" : "false";
  j += ",\"server_open\":";              j += hkServerOpen() ? "true" : "false";
  j += ",\"sites_open\":";               j += hkSitesOpen ? "true" : "false";
  j += "}}";
  web.send(200, "application/json", j);
}

void webTask(void* param) {
  for (;;) {
    web.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void startWeb() {
  static bool started = false;
  if (started) return;
  started = true;
#if ENABLE_FAILOVER_API
  web.on("/failover", handleFailover);
#endif
#if ENABLE_POWER_API
  web.on("/power", handlePower);
#endif
  web.on("/state", HTTP_GET, handleState);
  web.on("/state.json", HTTP_GET, handleStateJson);
  web.onNotFound([]() { web.send(404, "text/plain", "not found\n"); });
  web.begin();
  xTaskCreatePinnedToCore(webTask, "webTask", 6144, nullptr, 1, nullptr, 0);
  Serial.println("[+] Web endpoints ready: http://" + WiFi.localIP().toString() + "/ (/state, /state.json"
#if ENABLE_FAILOVER_API
                 ", /failover"
#endif
#if ENABLE_POWER_API
                 ", /power"
#endif
                 ")");
}
#endif  // ENABLE_FAILOVER_API || ENABLE_POWER_API

#if ENABLE_FAILOVER_API

void handleFailoverPending() {
  uint8_t p = failoverPending;
  if (!p) return;
  failoverPending = 0;
  if (p == 1) {
    notifyAlert(failoverOnMsg());
  } else {
    notifyAlert("FAILOVER OFF: 443 is back on mainhomeserver\n\n"
           "The iMac handed 192.168.1.254 back at " + fmtTime(nowEpoch()) + ".\n"
           "Failed over since " + fmtTime(failoverSince) + ".");
  }
}
#endif  // ENABLE_FAILOVER_API

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

#if ENABLE_HOMEKIT
// ---------------------------------------------------------------- HomeKit
// HomeSpan 1.9.1 (the last release for Arduino core 2.x). One bridge-less
// accessory with two contact sensors. HomeSpan polls in its own task
// (autoPoll), so loop() blocking in a check timeout cannot stall pairing or
// event delivery. All characteristic writes happen in that task, from the
// sensors' loop() methods below, which only read the flags set above.

#define FW_VERSION "2.0.0"   // shown as HomeKit firmware revision (HAP wants x.y.z); the HTTP user agent says 2.0
static const uint16_t HOMEKIT_PORT = 8081;   // HomeSpan's default is 80, which the web server already holds

// Mirrors HomeSpan's own rejects (00000000, 11111111 ... 99999999, 12345678,
// 87654321); anything else but 8 digits is refused too. setPairingCode() would
// halt the board on a bad code, so the check is done first.
static bool hkCodeValid(const char* c) {
  if (!c || strlen(c) != 8) return false;
  bool same = true;
  for (uint8_t i = 0; i < 8; i++) {
    if (!isdigit((uint8_t)c[i])) return false;
    if (c[i] != c[0]) same = false;
  }
  if (same) return false;                                   // includes the 00000000 placeholder
  return strcmp(c, "12345678") && strcmp(c, "87654321");
}

// A contact sensor that follows a bool source. closed (DETECTED, 0) = OK, open (NOT_DETECTED, 1) = problem.
struct HkContactSensor : Service::ContactSensor {
  Characteristic::ContactSensorState state{0};
  bool (*isOpen)();
  HkContactSensor(const char* name, bool (*src)()) : Service::ContactSensor(), isOpen(src) {
    new Characteristic::Name(name);
  }
  void loop() override {                                    // runs in the HomeSpan task
    int want = isOpen() ? 1 : 0;
    if (state.getVal() != want) state.setVal(want);
    hkPaired = HAPClient::nAdminControllers() > 0;
  }
};

static bool hkSitesSource() { return hkSitesOpen; }

void startHomeKit() {
  static bool tried = false;
  if (tried) return;
  tried = true;

#ifndef HOMEKIT_SETUP_CODE
  Serial.println("[-] HomeKit not started: HOMEKIT_SETUP_CODE is not defined in secrets.h.");
  return;
#else
  if (!hkCodeValid(HOMEKIT_SETUP_CODE)) {
    Serial.println("[-] HomeKit not started: HOMEKIT_SETUP_CODE in secrets.h is the placeholder or too simple "
                   "(need 8 digits, not all the same, not 12345678/87654321).");
    return;
  }

  homeSpan.setLogLevel(0);
  homeSpan.setPortNum(HOMEKIT_PORT);
  // 1.9.1 only brings up its HAP server and mDNS once it sees WiFi connected
  // through *its* credentials, so it gets the same ones this firmware uses. It
  // never starts an access point or asks for credentials.
  homeSpan.setWifiCredentials(ssid, pw);
  homeSpan.setSketchVersion(FW_VERSION);
  // No status LED, no control button: the RGB LED belongs to ledTask. No
  // enableOTA() (ArduinoOTA is ours) and no enableWebLog() (the web server is ours).
  homeSpan.begin(Category::Sensors, "Server Pinger", "esp32-pinger", "ESP32-S3 Server Pinger");
  homeSpan.setPairingCode(HOMEKIT_SETUP_CODE);              // after begin(); before polling starts

  static char serial[20];
  snprintf(serial, sizeof(serial), "%s", WiFi.macAddress().c_str());

  new SpanAccessory();
    new Service::AccessoryInformation();
      new Characteristic::Identify();
      new Characteristic::Name("Server Pinger");
      new Characteristic::Manufacturer("DIY");
      new Characteristic::Model("ESP32-S3 Server Pinger");
      new Characteristic::SerialNumber(serial);
      new Characteristic::FirmwareRevision(FW_VERSION);
    new HkContactSensor("Server", hkServerOpen);
    new HkContactSensor("Websites", hkSitesSource);

  // Core 0, priority 1: the LED task (priority 2) preempts it, so the LED
  // rhythm survives the seconds of big-number maths in pairing; web and OTA
  // tasks (priority 1) share time with it. Core 0 is also the core HomeSpan
  // removes the idle watchdog from, which pairing needs.
  homeSpan.autoPoll(8192, 1, 0);
  hkStarted = true;
  Serial.println("[+] HomeKit ready: port " + String(HOMEKIT_PORT));
#endif
}
#endif  // ENABLE_HOMEKIT

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
#if ENABLE_TG_COMMANDS
  tgOffset = (long)prefs.getULong("tgOffset", 0);
#endif

  Serial.printf("\n[*] Boot #%lu, reason: %s\n",
                (unsigned long)bootCount, resetReasonStr(bootReason));

  for (uint8_t i = 0; i < SITE_COUNT; i++) {
    siteState[i] = { true, 0, 0, 0, 0, 0, 0, 0, 0 };
  }

  currentState = WIFI_DOWN;
  startLEDs();

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
    Serial.print(".");
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nConnected! IP: " + WiFi.localIP().toString());
    syncTime(10000);

    startOTA();
#if ENABLE_FAILOVER_API || ENABLE_POWER_API
    startWeb();
#endif
#if ENABLE_HOMEKIT
    startHomeKit();
#endif
    doBootReportIfPending();
    currentState = WIFI_CONNECTED_IDLE;
  } else {
    Serial.println("\n[-] Could not join WiFi in 30s, continuing in retry mode.");
  }

  lastHeartbeatMs = millis();
}

void loop() {
  // While an image is streaming in, keep off the network and out of the heap.
  if (otaInProgress) {
    delay(20);
    return;
  }

  handleWiFi();

  if (WiFi.status() == WL_CONNECTED) {
    startOTA();
#if ENABLE_FAILOVER_API || ENABLE_POWER_API
    startWeb();
#endif
#if ENABLE_HOMEKIT
    startHomeKit();
#endif
#if ENABLE_FAILOVER_API
    handleFailoverPending();
#endif
#if ENABLE_POWER_API
    handlePowerPending();
#endif
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
    } else {
      readVerdict(false);        // idle between cycles: at most every VERDICT_POLL_MS
    }
    powerTick();                 // ends a planned-power mute when its conditions hold
    applyVerdictToLed();         // also lets an old "warning" expire after 120 s
    publishState();
  }

  heartbeat();
  delay(20);
}
