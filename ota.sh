#!/usr/bin/env bash
#
# ota.sh - build and flash the Server Pinger over WiFi.
#
#   ./ota.sh                # default IP below
#   ./ota.sh 192.168.1.42   # somewhere else
#
# ---------------------------------------------------------------------------
# WHY NOT JUST `pio run -e ota -t upload`?
# ---------------------------------------------------------------------------
# You can, and it is the same protocol. But on macOS it usually fails, and the
# failure is silent and misleading.
#
# OTA is a *callback* protocol: the Mac sends a UDP invitation to the board on
# port 3232 saying "connect back to me on port N", and the ESP32 then opens an
# *inbound* TCP connection to the Mac to pull the firmware.
#
# That inbound connection is judged by the macOS Application Firewall. For an
# interpreter it does not recognise - PlatformIO's bundled Python, miniconda,
# a venv - macOS completes the TCP handshake and then immediately closes the
# socket before the uploader's accept() ever returns. The board sits there,
# receives nothing, and dies after its receive timeout with:
#
#     ArduinoOTA.cpp:391 _runUpdate(): Receive Failed     (error 3)
#
# which looks exactly like a firmware bug and is not one. Confirmed by watching
# netstat: the connection reaches the correct port already in FIN_WAIT_2.
#
# /usr/bin/python3 is on the system allowlist, so its inbound connections
# survive. This script therefore builds with PlatformIO but runs the upload
# step under /usr/bin/python3.
#
# To use `pio run -e ota -t upload` directly instead, allow PlatformIO's
# Python through the firewall once:
#
#   sudo /usr/libexec/ApplicationFirewall/socketfilterfw \
#        --add ~/.platformio/penv/bin/python
#   sudo /usr/libexec/ApplicationFirewall/socketfilterfw \
#        --unblockapp ~/.platformio/penv/bin/python
#
# Note `socketfilterfw --getappblocked` reports "permitted" even for binaries
# that are actually blocked, so do not trust it as a check.
# ---------------------------------------------------------------------------

set -euo pipefail

DEVICE_IP="${1:-192.168.1.220}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PIO="$HOME/.platformio/penv/bin/pio"
ESPOTA="$HOME/.platformio/packages/framework-arduinoespressif32/tools/espota.py"
FIRMWARE="$ROOT/.pio/build/usb/firmware.bin"
PYTHON=/usr/bin/python3

for f in "$PIO" "$ESPOTA" "$PYTHON"; do
  [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

echo "==> Building"
"$PIO" run -e usb

echo "==> Uploading to $DEVICE_IP over WiFi"
"$PYTHON" "$ESPOTA" -i "$DEVICE_IP" -p 3232 -f "$FIRMWARE" -r

echo
echo "==> Done. The board reboots into the new firmware and sends you a"
echo "    Telegram 'PINGER ONLINE' with reset reason 'software restart'."
