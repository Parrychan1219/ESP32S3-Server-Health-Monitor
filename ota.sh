#!/usr/bin/env bash
#
# ota.sh - compile and flash the Server Pinger over WiFi. No USB cable needed.
#
#   ./ota.sh              # uses the default IP below
#   ./ota.sh 192.168.1.42 # or point it somewhere else
#
# ---------------------------------------------------------------------------
# WHY THIS USES /usr/bin/python3 AND NOT plain `python3`
# ---------------------------------------------------------------------------
# OTA is a callback protocol. The Mac sends a UDP invitation to the board on
# port 3232 telling it "connect back to me on port N", and the ESP32 then opens
# a TCP connection *inbound* to the Mac to pull the firmware.
#
# That inbound connection is subject to the macOS Application Firewall. For a
# binary the firewall does not recognise, macOS completes the TCP handshake and
# then immediately closes the socket before the uploader's accept() ever sees
# it. The board sits there, receives nothing, and dies with:
#
#     ArduinoOTA.cpp:391 _runUpdate(): Receive Failed   (error 3)
#
# which looks exactly like a firmware bug and is not one. /usr/bin/python3 is
# on the system allowlist, so its inbound connections survive.
#
# If you ever want to use a different interpreter (miniconda, a venv, pyenv),
# allow it through the firewall once:
#
#   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --add /path/to/python3
#   sudo /usr/libexec/ApplicationFirewall/socketfilterfw --unblockapp /path/to/python3
# ---------------------------------------------------------------------------

set -euo pipefail

DEVICE_IP="${1:-192.168.1.220}"
SKETCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="$SKETCH_DIR/build-ota"
CLI="/Applications/Arduino IDE.app/Contents/Resources/app/lib/backend/resources/arduino-cli"
CORE="$HOME/Library/Arduino15/packages/esp32/hardware/esp32/3.3.10"
ESPOTA="$CORE/tools/espota.py"
FQBN="esp32:esp32:esp32s3:FlashSize=8M,PartitionScheme=default_8MB,DebugLevel=error"
PYTHON=/usr/bin/python3

for f in "$CLI" "$ESPOTA" "$PYTHON"; do
  [ -e "$f" ] || { echo "missing: $f" >&2; exit 1; }
done

echo "==> Compiling ($FQBN)"
"$CLI" compile --fqbn "$FQBN" --output-dir "$BUILD_DIR" "$SKETCH_DIR"

# Pick up an OTA password from secrets.h if one is defined there.
AUTH=()
if [ -f "$SKETCH_DIR/secrets.h" ]; then
  PWD_LINE=$(grep -E '^[[:space:]]*#define[[:space:]]+OTA_PASSWORD' "$SKETCH_DIR/secrets.h" || true)
  if [ -n "$PWD_LINE" ]; then
    AUTH=(-a "$(echo "$PWD_LINE" | sed -E 's/.*"(.*)".*/\1/')")
    echo "==> Using OTA password from secrets.h"
  fi
fi

echo "==> Uploading to $DEVICE_IP over WiFi"
"$PYTHON" "$ESPOTA" -i "$DEVICE_IP" -p 3232 -f "$BUILD_DIR/main.ino.bin" -r ${AUTH[@]+"${AUTH[@]}"}

echo
echo "==> Done. The board reboots into the new firmware and sends you a"
echo "    Telegram 'PINGER ONLINE' with reset reason 'software restart'."
