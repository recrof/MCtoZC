# SPDX-License-Identifier: MIT
# MeshCore -> ZephCore repeater migration firmware (nRF52840, Adafruit UF2/OTA bootloader)
#
#   SoftDevice v6 (RAK4631, T-Echo, Heltec T114, ...):
#     west build -b nrf52840dk/nrf52840 <this dir> -d build_v6 -- -DDTC_OVERLAY_FILE=boards/sdv6.overlay
#   SoftDevice v7 (T1000-E, Wio Tracker L1, XIAO nRF52840, ...):
#     west build -b nrf52840dk/nrf52840 <this dir> -d build_v7 -- -DDTC_OVERLAY_FILE=boards/sdv7.overlay
#
# nrf52840dk is only the nRF52840 base; the overlay supplies the flash map, so one
# image per SoftDevice covers every board on it (as ZephCore's formatter does).
# The base must run no board code at boot: Zephyr's rak4631 board.c, for one,
# writes UICR REGOUT0 (the GPIO/VDD rail voltage) on any board in high-voltage
# mode, and that stays until a full chip erase.
# Use build.sh, which builds both and packages the DFU zips.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(mc_to_zc_migrator)

target_sources(app PRIVATE
  src/main.c
  src/lfs1/lfs1.c
  src/lfs1/lfs1_util.c
)
target_include_directories(app PRIVATE src src/lfs1)
# Vendored littlefs v1 is untouched upstream code; keep its warnings out of the log.
set_source_files_properties(src/lfs1/lfs1.c src/lfs1/lfs1_util.c PROPERTIES
  COMPILE_OPTIONS "-Wno-sign-compare;-Wno-unused-parameter;-Wno-type-limits")
