# ESP32-S3 Server Pinger

A small always-on watchdog for a home server and a handful of public sites.
Every minute it pings the LAN server, checks each site over HTTPS, and reports
anything that changes to Telegram. It also remembers what happened while it was
offline, so a power cut or an internet outage is reported *after* the fact with
a real start time and duration rather than just going quiet.

An RGB LED on the board shows the current state at a glance.

## What it watches

| Target | How | Alert |
|---|---|---|
| LAN server | ICMP ping, 3 strikes | Down / recovered, with downtime |
| Public sites | HTTPS request per cycle | Down / recovered, with HTTP code and latency |
| Internet | Ping `1.1.1.1` and `8.8.8.8` | Outage start, duration, recovery |
| WiFi | Association state | Folded into the internet outage report |
| Mains power | NVS heartbeat + reset reason | Reported on the next boot |

## Alerts it sends

- **Site down / up** — URL, when it first failed, when that was confirmed, how
  long it was down, and the HTTP status or connection error.
- **Internet restored** — when it went down, when it came back, total offline
  time, and what the failure looked like.
- **Power outage** — last time the board was alive, when power returned, and
  roughly how long it was dark.
- **Boot report** — reset reason, boot count, and what it is now watching.
- **Daily digest** at 09:00 — uptime, outage count, per-site availability.
- **Failover on / off** — when the iMac takes over port 443 from the server
  (and when it hands it back). The iMac reports it; see below.

Alerts raised while the connection is down are queued (up to 12) and sent once
it comes back, so an outage never swallows its own notification.

## Hardware

- ESP32-S3 dev board, **8MB flash**, with an addressable RGB LED on GPIO 48
- USB is a CH343 UART bridge, so serial is UART0 and *USB CDC On Boot stays off*
- Static IP `192.168.1.220`

## Repository layout

```
.
├── platformio.ini          board, partitions, libraries, OTA target
├── ota.sh                  build + flash over WiFi
├── README.md
└── src/
    ├── main.cpp            the firmware
    ├── sites.h             >>> the list of sites to check <<<
    ├── secrets.example.h   template
    └── secrets.h           your credentials (gitignored)
```

## Getting started

This is a **PlatformIO** project. Arduino IDE and arduino-cli are not used and
are not required. Install the *PlatformIO IDE* extension in VS Code, open this
folder, and it will pick everything up.

**1. Credentials**

```bash
cp src/secrets.example.h src/secrets.h
```

Then fill in your bot token, chat ID and WiFi details. `secrets.h` is
gitignored; keep it that way.

**2. Sites to monitor**

Edit `src/sites.h`. One line per site, no other file needs changing:

```c
SITE("moodle.sy19.org", "https://moodle.sy19.org/")
SITE("dllmch.org",      "https://dllmch.org/")
```

A site counts as **up** on any HTTP status below 500, so 200, 301, 401 and 403
all mean "the server answered". 502/503/504, a refused connection, a DNS
failure or a TLS error mean **down**. If a site normally sits behind auth and
returns 401, that still counts as up — point the URL at a health endpoint if
you need something stricter.

**3. Network**

The static IP, gateway and the LAN server address are near the top of
`src/main.cpp`. The timezone is a POSIX TZ string, `HKT-8` by default.

## Building and flashing

**Over USB** (required the first time — OTA cannot bootstrap itself):

```bash
pio run -e usb -t upload            # or the VS Code PlatformIO buttons
pio device monitor                  # 115200 baud
```

**Over WiFi**, every time after that:

```bash
./ota.sh                 # or ./ota.sh <ip>
```

You will know it worked without watching serial: the board reboots and sends a
Telegram `PINGER ONLINE` with reset reason `software restart`. A failed or
interrupted OTA cannot brick it — the bootloader just keeps running the old
slot.

### If OTA hangs at 0% on macOS

This one is worth knowing about, because the symptom points at the wrong place.

OTA is a *callback* protocol. Your Mac sends a UDP invitation to the board on
port 3232 saying "connect back to me on port N", and the ESP32 then opens an
**inbound** TCP connection to your Mac to pull the firmware. That inbound
connection is judged by the macOS Application Firewall, and for an interpreter
it does not recognise — PlatformIO's bundled Python, miniconda, a venv — macOS
completes the TCP handshake and then immediately closes the socket before the
uploader's `accept()` ever returns.

The board therefore receives nothing and fails with:

```
ArduinoOTA.cpp:391 _runUpdate(): Receive Failed      (error 3)
```

which looks exactly like a firmware bug and is not one.

`ota.sh` avoids this by running the upload step under `/usr/bin/python3`, which
is on the system allowlist. If you would rather use `pio run -e ota -t upload`
directly, allow PlatformIO's Python through once:

```bash
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add ~/.platformio/penv/bin/python
sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp ~/.platformio/penv/bin/python
```

Note that `socketfilterfw --getappblocked` reports "permitted" even for
binaries that are in fact blocked, so do not trust it as a check.

## LED reference

**Steady**

| Colour | Meaning |
|---|---|
| 🔴 Red | WiFi down |
| 🟣 Purple | WiFi connected, idle |
| 🟡 Yellow | Server missed its last ping |
| 🟢 Green | Server normal |

**Flashing**

| Colour | Meaning |
|---|---|
| 🔴 Red | Server down, internet up |
| 🟣 Purple | Internet down, WiFi up |
| 🔵 Blue (slow) | One or more sites are down, internet up |
| 🔵 Blue (fast) | OTA update in progress |

The LED is driven by its own FreeRTOS task on core 0, not by the main loop, so
it keeps its rhythm while a check cycle is stuck in a timeout. The blink phase
is taken from the clock rather than toggled, and the current colour is re-sent
every second even when it has not changed, so a missed write cannot leave the
LED dark. (Before 2026-09-24 the loop drove it, and a flash froze -- sometimes
in its "off" half -- for as long as a check blocked.)

## Telegram commands

| Command | Does |
|---|---|
| `/status` | Uptime, boot count, outage totals, per-site availability, RSSI, free heap |
| `/server` | Details from the server itself — uptime, load, CPU temperature, memory, disks, RAID, containers, failed services, power estimate. Fetched over the LAN from `http://192.168.1.200/pinger/status.txt`, which the server regenerates every minute (see its manual, §9) |
| `/check` | Runs a check cycle immediately instead of waiting for the next one |
| `/reboot` | Restarts the board |
| `/help` | Lists the commands and what they do |

The menu Telegram shows when you type `/` is a separate list stored on
Telegram's side (Bot API `setMyCommands`, or BotFather's `/setcommands`) — the
firmware does not register it. When a command is added here, add it there too
(`/server` was added and all five descriptions rewritten to match this table on 2026-09-14).

Anything else — a typo, an unknown slash command, plain text, or a message with
no text at all such as a sticker — gets a short "not a command" reply pointing
at `/help`. `/start`, which Telegram sends when the chat is first opened, shows
the help.

Only messages from the configured chat ID are obeyed. Commands are picked up
every `TG_POLL_MS` (5s), so a reply can lag by that much.

## How the trickier parts work

**Internet outages.** The start is timestamped at the first failed check and
also written to NVS, so a reboot in the middle of an outage does not lose it.
Duration is measured with `millis()` rather than wall clock, so NTP drift
cannot distort it.

**Power outages.** The board cannot know at the instant power dies, so it
writes a timestamp to NVS every 60 seconds. On boot it reads `esp_reset_reason()`;
a power-on or brownout reset plus a stored heartbeat means the power was cut,
and the gap between the last heartbeat and this boot is the outage — accurate
to within one heartbeat interval. The restore time is back-computed as
`now - uptime` rather than "now", because the router usually takes longer to
come back than the board does, and using "now" would inflate the number. The
report is deferred until there is both a connection and a synced clock, since a
mains cut normally takes the router down too.

**OTA reliability.** `ArduinoOTA.handle()` runs in its own FreeRTOS task pinned
to core 0. The main loop can block for 20-30 seconds while HTTP checks time
out, which is long enough for the uploader to give up on the handshake — and
that is exactly when you want to push a fix.

**Failover reports (added 2026-09-24).** The router forwards 443 to
`192.168.1.254`, an address the server holds while its nginx is healthy; when it
is not, the iMac takes the address and serves a "temporarily down" page
(project `~/Projects/mhs-failover` on the iMac). The iMac has no bot token, so
it reports each change here with `POST http://192.168.1.220/failover?state=on`
or `state=off`, and the board sends the Telegram message. Only `192.168.1.202`
and `.254` are accepted (403 otherwise), and only a change of state produces a
message, so a repeated report is harmless. Like OTA, the endpoint is served
from its own task on core 0; the handler only records the change and `loop()`
sends the message, so two TLS clients never run at once. While failed over,
`/status` shows a `Failover:` line. The board is **not** part of the failover
decision -- if it is offline the takeover still happens, only the message is lost.

## Configuration reference

Tunables live near the top of `src/main.cpp`:

| Setting | Default | Meaning |
|---|---|---|
| `PING_INTERVAL` | 60 s | Main check cycle |
| `SITE_TIMEOUT_MS` | 8 s | Per-site HTTP timeout |
| `SITE_FAIL_STRIKES` | 2 | Consecutive failures before "down" |
| `SERVER_FAIL_STRIKES` | 3 | Missed pings before the server is "down" |
| `HEARTBEAT_MS` | 60 s | NVS write interval, sets power-outage resolution |
| `REBOOT_AFTER_MS` | 15 min | Self-reboot if WiFi never returns |
| `TZ_INFO` | `HKT-8` | POSIX timezone string |
| `DIGEST_HOUR` | 9 | Hour for the daily report |
| `TG_POLL_MS` | 5 s | How often Telegram is polled for commands |

Keep the site list under about six entries, or a cycle full of timeouts will
run past the next cycle.

## A note on the Arduino core version

`platformio.ini` uses the official `espressif32` platform, which pins **Arduino
core 2.0.17**. The code also builds and runs on core 3.3.10; 2.0.17 is simply
what the official platform ships, and it is fully cached so builds work
offline.

If you want core 3.x, the official platform will not give it to you — use the
community `pioarduino` fork instead by replacing the platform line:

```ini
platform = https://github.com/pioarduino/platform-espressif32/releases/download/<tag>/platform-espressif32.zip
```

Pick `<tag>` from the latest release at
<https://github.com/pioarduino/platform-espressif32/releases>. Expect a large
one-time download. Nothing in the source needs to change.

## Troubleshooting

| Symptom | Cause |
|---|---|
| OTA fails with `Receive Failed` / error 3 | macOS firewall, see above. Not the firmware. |
| `pio device monitor` shows nothing | Wrong port. The CH343 enumerates as `/dev/cu.usbmodem*`. |
| Board never joins WiFi | Static IP clashes with the router's DHCP pool, or wrong SSID. LED stays red. |
| Timestamps say "clock not synced" | NTP has not reached a server yet; outage durations still work. |
| A site flaps up and down | Raise `SITE_FAIL_STRIKES` or `SITE_TIMEOUT_MS`. |
