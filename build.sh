#!/usr/bin/env bash
# SPDX-License-Identifier: MIT
# Builds the MeshCore -> ZephCore migrator for SoftDevice v6 and v7 and packages
# each as UF2 (drag & drop) and as an Adafruit BLE/serial DFU zip.
#
#   ./build.sh            # both
#   ./build.sh 6|7        # one
#
# Needs the ZephCore west workspace (for Zephyr itself) and the Zephyr SDK:
#   ZEPHCORE_WS   default ~/Documents/ZephCore
#   ZEPHYR_SDK_INSTALL_DIR  default ~/zephyr-sdk-1.0.1
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
WS="${ZEPHCORE_WS:-$HOME/Documents/ZephCore}"
export ZEPHYR_SDK_INSTALL_DIR="${ZEPHYR_SDK_INSTALL_DIR:-$HOME/zephyr-sdk-1.0.1}"
OUT="$HERE/out"
mkdir -p "$OUT"

# Adafruit DFU zip: --sd-req is what stops the bootloader accepting the wrong
# image for its SoftDevice (v6 = 0x00B6, v7 = 0x0123). adafruit-nrfutil from PATH,
# else PlatformIO's bundled copy.
declare -A SDREQ=( [6]=0x00B6 [7]=0x0123 )
nrfutil_genpkg() {  # <sd-req> <hex> <zip>
	if command -v adafruit-nrfutil >/dev/null; then
		adafruit-nrfutil dfu genpkg --dev-type 0x0052 --sd-req "$1" --application "$2" "$3"
	else
		local t="$HOME/.platformio/packages/tool-adafruit-nrfutil"
		PYTHONPATH="$t/site-packages" python3 "$t/adafruit-nrfutil.py" \
			dfu genpkg --dev-type 0x0052 --sd-req "$1" --application "$2" "$3"
	fi
}

build_one() {
	local v=$1 bd="$HERE/build_v$1"
	( cd "$WS" && west build -b nrf52840dk/nrf52840 "$HERE" --pristine -d "$bd" -- \
		-DDTC_OVERLAY_FILE="$HERE/boards/sdv$v.overlay" )
	cp "$bd/zephyr/zephyr.uf2" "$OUT/MeshCore_to_ZephCore_SoftDevice_v${v}.uf2"
	nrfutil_genpkg "${SDREQ[$v]}" "$bd/zephyr/zephyr.hex" "$OUT/MeshCore_to_ZephCore_SoftDevice_v${v}.zip"
	echo "==> $OUT/MeshCore_to_ZephCore_SoftDevice_v${v}.{uf2,zip}"
}

for v in ${1:-6 7}; do build_one "$v"; done
