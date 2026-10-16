# Wall dashboard ⇄ ESP32 pinger — interface

The same file is in both repositories. Change it in both, in the same session as the code.

- Wall dashboard: `Parrychan1219/Dashboard-as-a-Webapp` (server `wall-status.py` + page `/wall/`)
- ESP32 pinger: `Parrychan1219/ESP32S3-Server-Health-Monitor` (firmware on `192.168.1.220`)

## Why it exists

On 2026-10-16 at about 21:50 HKT the public sites failed on the internet path only (public DNS → WAN IP →
router → nginx). The ESP32 saw it and flashed blue. The wall page showed NORMAL, because the server's
`service-health-monitor` examines the sites from inside the server (`192.168.1.200:443`).

Since then:

- **The server is the master** of one combined verdict. `wall-status.py` calculates it.
- **The ESP32 is a sensor** for the outside view (public sites, internet, can it reach the server) and the
  **fallback** when the server does not answer.

Both directions use plain HTTP by IP on the LAN. No name resolution, no TLS: either side must keep working
when the server's DNS (AdGuard) or nginx TLS is down.

## A. ESP32 → server: `GET http://192.168.1.220/state.json`

- LAN only (remote IP `192.168.1.x`, else 403). The older text `/state` stays as it is.
- Headers: `Access-Control-Allow-Origin: *`, `Cache-Control: no-store`, `Content-Type: application/json`.
- Body: one JSON object, no secrets, under 1500 bytes.

```json
{
  "v": 1,
  "uptime_s": 12645,
  "now": 1792160910,
  "wifi_rssi": -58,
  "internet": true,
  "server_up": true,
  "monitor_up": true,
  "led": "site_down",
  "cycle_ts": 1792160880,
  "verdict_level": "normal",
  "verdict_age_s": 7,
  "sites": [
    {"name": "dllmch.org", "up": false, "code": -1, "fails": 3, "down_since": 1792160500}
  ]
}
```

| Field | Meaning |
|---|---|
| `now` | Board epoch time, 0 if the clock is not synced |
| `cycle_ts` | Epoch when the last check cycle ended, 0 = none yet |
| `internet` | The board's own internet check (1.1.1.1 / 8.8.8.8) in the last cycle |
| `server_up`, `monitor_up` | The board's `serverUp` and `monitorUp` |
| `led` | `wifi_down`, `wifi_idle`, `no_internet`, `server_up`, `server_missed`, `server_dead`, `site_down`, `ota`, `server_warning` |
| `verdict_level` | Last `level_server` read from part B, or `unknown` (never read, or older than 120 s) |
| `verdict_age_s` | Seconds since that read, -1 = never |
| `sites[]` | One per entry in `sites.h`, in order. `up` = the board's UP/DOWN after its strike rules; `code` = last HTTP code, negative = transport error; `down_since` = epoch of the first failure of the current outage, 0 when up |

## B. Server → ESP32: `GET http://192.168.1.200/pinger/verdict.txt`

- File `/var/lib/wall-status/verdict.txt`, written atomically by `wall-status.py` every 5 s.
- nginx vhost `server-status`, `location = /pinger/verdict.txt`, `text/plain`, `Cache-Control: no-store`.

```
level=warning
level_server=caution
count=2
ts=1792160930
---
bad dllmch.org down from outside (ESP32)
warn seafile not running
```

- `level`: `warning` if any item is `bad`, `caution` if any is `warn`, else `normal`.
- `level_server`: the same rule, counting only items that did **not** come from the ESP32 (table C).
  The ESP32 uses this one, so its own report never comes back to it as orange.
- `count`: number of items. `ts`: epoch when written. Then `---` and up to 5 lines `<bad|warn> <text>`, worst first, ≤ 80 chars.

## C. Verdict items (`status.json` → `verdict`)

```json
"verdict": {"level": "warning", "items": [{"text": "…", "state": "bad"}], "esp32": {…part A…} | null, "esp32_age": 3}
```

Items = temperature, disks, md0, service-health-monitor lines, failed units, Docker, job warnings (thresholds:
`LIMITS` in `wall-status.py`), plus these ESP32 items:

| Condition | Text | State |
|---|---|---|
| ESP32 answers, `internet` false | `No internet (ESP32)` | bad |
| ESP32 answers, a site has `up` false | `<name> down from outside (ESP32)` | bad |
| ESP32 answers, `server_up` false | `ESP32 cannot reach the server` | warn |
| No good answer for ≥ 60 s | `ESP32 pinger not answering` | warn |

`bad` first, then `warn`. The page shows these items and adds only its own `Data N s old`.

## D. ESP32 LED

`SERVER_REPORTS_WARNING` = **orange flash** (`255, 80, 0`, 800 ms). Only in place of green (`SERVER_UP`), only
when the last `level_server` read is `warning`, not older than 120 s, and its `ts` is not older than 120 s
(when the board clock is synced). Every other state keeps its colour and wins over orange. `caution` stays
green. The board sends no Telegram message about the verdict; the server sends its own alerts.

## E. "ESP32 confirms" (wall page)

After 60 s with no answer from the server, the page reads part A directly (3 s timeout, every 10 s) and shows
under NO ANSWER:

- `server_up` false → `SERVER DOWN — ESP32 confirms`
- `server_up` true → `ESP32 reaches the server — check this Wi-Fi`
- each site with `up` false → `<name> unreachable (ESP32)`
- no answer → `ESP32 not answering either — check the Wi-Fi`

`?esp=<url>` overrides the ESP32 URL for tests.
