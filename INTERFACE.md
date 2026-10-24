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
| `power` | Planned power action the board knows about (part F), or `null`: `{"type": "reboot"\|"shutdown", "ts": <server epoch of the signal>, "until": <epoch the reboot mute ends at the latest, 0 for shutdown or unsynced clock>, "test": false}`. Also in `/state` as `power=<type\|none>` |
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
  The wall phone's own battery item (`Wall phone battery N %`, from 2026-10-25) is also left out of it:
  it counts for `level` and the wall page only, never for the LED.
- `power`: `reboot`, `shutdown` or `none` — whether a planned power signal (part G) is still in force on the server.
  Placed after `ts`. The ESP32 uses it to end its mute (part F). Readers that do not know it ignore it.
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

## F. Server → ESP32: planned reboot / shutdown (`POST http://192.168.1.220/power`)

Design: `Dashboard-as-a-Webapp/docs/superpowers/specs/2026-10-18-power-signal-design.md`. Reboot and shutdown are
two separate signals. They are never combined.

- `POST /power?type=reboot|shutdown|clear&ts=<epoch>&window=<s>&settle=<s>[&test=1]`, no body.
- Only from `192.168.1.200` (else 403, nothing changes). Bad or missing `type`, or `ts`/`window`/`settle` not a
  positive integer when needed → 400.
- 200 `application/json`: `{"ok":true,"type":"reboot","until":<epoch or 0>,"test":false}`.
- The server sends `window=360`, `settle=300` today. The board uses the values it receives, timed with `millis()`
  from the moment it receives the POST (its own clock can be unsynced). `ts` is for display only.
- A new signal replaces the old one. `clear` ends the mute at once and sends nothing (tests use it).

**Telegram, sent at once on receipt** (prefix `TEST ` on each title when `test=1`):

```
SERVER REBOOTING: mainhomeserver

Signal: HH:MM:SS
Alerts muted until HH:MM:SS at the latest (6 min). If it is not back by then, you get SERVER DOWN.
```
```
SERVER SHUTTING DOWN: mainhomeserver

Signal: HH:MM:SS
Alerts muted until it is back and settled.
```

**While muted** the board checks, keeps state, publishes `state.json` and drives the LED exactly as without a
signal. It only does not send: `SERVER DOWN/UP`, `SITE DOWN/UP`, `HEALTH MONITOR DOWN/UP`,
`SERVER ALERTS BLOCKED/FLOWING AGAIN`, `FAILOVER ON/OFF`. Internet/Wi-Fi messages, the daily report and command
replies are not muted. Messages muted are dropped, not queued.

**End of mute:**

| Signal | Ends when (first of) | Then |
|---|---|---|
| reboot | the server pings **and** a fresh `verdict.txt` (part B, `ts` ≤ 120 s old) says `power=none`; or `window` s have passed | see below |
| shutdown | the server pings **and** a fresh `verdict.txt` says `power=none`; or the server has pinged for `settle` s | see below |

No time limit while the server does not ping after a shutdown (user's decision, 2026-10-18).

- If the server pings at the end: one message `SERVER BACK after reboot` / `SERVER BACK after shutdown` with
  `Signal: HH:MM:SS`, `Down for: <duration>` (first missed ping → first good ping; `0s` if no ping was missed).
  Then, for each site, the health monitor and failover that are DOWN/ON **now**, the normal DOWN/ON message.
  Nothing for what went down and recovered inside the mute.
- If a reboot window ends and the server does not ping: the normal `SERVER DOWN: mainhomeserver` message with
  the extra line `Did not come back after its reboot signal. Last seen HH:MM:SS with reboot signal.`
  (`last seen` = last good ping), and normal behaviour from then on.
- The mute lives in RAM. A board restart during a mute ends it (the board then alerts normally).

## G. Server side of the signal

- `power-signal.service` (oneshot, `RemainAfterExit=yes`, enabled) runs `/usr/local/sbin/power-signal.py stop`
  as `ExecStop`. Ordered `After=network-online.target nginx.service docker.service wall-status.service`, so at
  shutdown it runs before those stop. Type from the queued job: `reboot.target`/`kexec.target` → `reboot`,
  `poweroff.target`/`halt.target` → `shutdown`, else no signal.
- `/var/lib/power-signal/signal.txt` (0644), written atomically, persists across the boot:

```
type=reboot
ts=1792262400
window=360
settle=300
boot_id=6c1f…
test=0
```

  `type=none` after `--clear`. Served at `GET http://192.168.1.200/pinger/power.txt` (LAN only, `no-store`).
- **In force** (wall-status, monitor), all must hold: `type` is `reboot` or `shutdown`; `ts` ≤ 7 days old; and
  - same boot (`boot_id` = current), `test=0`: `now − ts ≤ 300` (the machine is going down; a slow container
    stop must not turn the wall red, and a cancelled shutdown stops showing after 5 min);
  - same boot, `test=1`: `now − ts ≤ window` (reboot) or until `--clear` (shutdown);
  - later boot, reboot: `now < ts + window` and not consumed;
  - later boot, shutdown: uptime < `settle` and not consumed.
  - **Consumed** = on a later boot, with uptime ≥ 180 s (before that, nothing has reported yet), the server's own items (without the power item and without ESP32 items) have
    no `bad` or `warn` left. wall-status records it in `/var/lib/wall-status/power-consumed` (the signal `ts`).
- `status.json` gains `"power": {"type", "ts", "until", "test"} | null`. (The watt reading that used this name
  until 2026-10-18 is now `power_monitor.watts`; `health.html` reads both.) (`until` = `ts + window` for reboot, 0 for
  shutdown). While in force, `verdict.items` starts with a `warn` item — `Server rebooting (since HH:MM:SS)`,
  `Server shutting down (since HH:MM:SS)` (same boot) or `Server starting after shutdown (since HH:MM:SS)` —
  and every service item (service-health-monitor lines, failed units, Docker, jobs, ESP32 items) that would be
  `bad` is `warn`. Hardware items (temperature, disks, md0) keep their state.
- `service-health-monitor` holds every alert while the signal is in force (it treats "consumed" as in force
  until uptime ≥ `BOOT_GRACE_SECONDS`), and sends what is still broken afterwards.
- If the ESP32 does not answer the POST (2 tries, 3 s), `power-signal.py` sends the announcement through
  `sendtg`, adding `The ESP32 did not acknowledge: its alerts are not muted.`

## H. Suppressed warnings (2026-10-22)

The user can suppress a red **service/site down** warning from the wall page for 15 min, 1 h, 8 h or
"until I change it". Cautions (yellow, `warn`) can never be suppressed. A suppressed warning does not
count for `level`/`level_server`, does not chime, and does not make the ESP32 LED blue or orange.
**Telegram is unchanged:** `service-health-monitor` and the ESP32 bot still send their DOWN/UP messages
(standing rule: never mute the user's alerts beyond what was asked).

**Key.** Every suppressible item has a `key`, the name of the thing that is down. Only `bad` items with a key
can be suppressed:

| Item | Key |
|---|---|
| Monitor line `<KIND> since HH:MM: <what>`, state bad (not `SLOW`) | first word of `<what>`, trailing `:` removed (`moodle.sy19.org: HTTP 503` → `moodle.sy19.org`; `fwupd-refresh.service failed` → `fwupd-refresh.service`; container lines → the container name) |
| `FAILED <unit>` | `<unit>` |
| `<name> down from outside (ESP32)` | `<name>` (the `sites[].name` of part A) |

Hardware items, jobs, `No internet (ESP32)`, `ESP32 …`, `Docker not answering`, `monitor silent`,
`Monitor N problems` and the power item have no key. One key covers every bad item with that key, so
suppressing `moodle.sy19.org` hides both the monitor line and the ESP32 line. Keys match
`^[A-Za-z0-9._@:+-]{1,120}$`; anything else has no key.

**Store.** `/var/lib/wall-status/suppress.json` (0644, written atomically by `wall-status.py` only):

```json
{"v": 1, "entries": [{"key": "moodle.sy19.org", "until": 0, "ts": 1792600000,
                       "text": "DOWN since 18:01: moodle.sy19.org: HTTP 503"}]}
```

`until` = epoch the suppression ends, `0` = until the user removes it. `ts` = when it was set. `text` = the
item text when it was set (for the list). An entry with `0 < until <= now` is dropped on the next tick. At most
50 entries. An entry stays when the problem clears (it is the user's to remove); a time-limited entry that
expires while the thing is still down brings the red warning (and chime) back.

**API (page → server).** `POST http://192.168.1.200/wall/suppress`, nginx `location = /wall/suppress` in vhost
`server-status` (inherits its LAN allow/deny) → `proxy_pass http://127.0.0.1:8137` (a thread in `wall-status.py`,
loopback only). Request header `Content-Type: application/json` required (else 415; this keeps other sites'
pages from posting cross-origin without a preflight, and no CORS headers are sent). Body ≤ 1024 bytes:

- `{"action": "suppress", "key": "<key>", "minutes": 15|60|480|null}` (`null` = until changed). The key must
  belong to a **current `bad` item** (else 409 `not a current red warning`). Setting an existing key replaces it.
- `{"action": "unsuppress", "key": "<key>"}` — removes the entry (404 if none).
- 200 `{"ok": true, "suppressed": [<entries as in verdict.suppressed>]}`; 400 bad JSON/action/key/minutes;
  405 not POST; 413 too big. After a change the daemon writes `status.json`/`verdict.txt` within ~1 s.
- Every change is logged to the journal: `suppress <key> for <15 min|1 h|8 h|until changed>` / `unsuppress <key>`.

**`status.json`.** `verdict.items[]` gain `"key"` on bad items that have one. Suppressed items are **removed** from
`verdict.items` and from the level. New `verdict.suppressed`: every entry, oldest first:
`{"key", "until", "ts", "text", "active"}` — `active` = a bad item with this key exists right now (`text` is then
the current text of the first such item).

**`verdict.txt` (part B).** Items and `count` exclude suppressed items. New line after `power`:
`suppressed=<key>,<key>,…` — all entries (active or not), in store order, empty value when none, at most
400 chars (whole keys only). Readers that do not know it ignore it.

**ESP32 (part D).** When the last good verdict read (≤ 120 s) lists a site's `name` in `suppressed`, that site's
DOWN does not set the blue `site_down` LED state (it still shows in `state.json` `sites[]` as `up: false`, and
Telegram SITE DOWN/UP is sent as before). If no fresh verdict is available, nothing is suppressed. So with every
red warning suppressed the LED is green.

**Page.** Each red item with a `key` has a `Suppress` button → chooser `15 min` / `1 hr` / `8 hr` /
`Until I change it` / `Cancel`. The footer gets a green `· N suppressed` (N = entries) right after
`updated HH:MM:SS`, on every screen that shows server data; tap → list of entries (key, until, `down now`/`OK now`)
each with `Unsuppress`, plus `Close`.
