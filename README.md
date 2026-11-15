# MeshCore → ZephCore repeater migrator (nRF52840)

One-shot firmware. Flash it over a running **MeshCore repeater** on an nRF52840 board; it converts
the repeater's saved state into the volume **ZephCore** reads, then reboots into **BLE OTA DFU** so
you can push the ZephCore repeater `.zip` without touching USB.

| File (`out/`) | Use on | Boards (per ZephCore `formatter/README.md`) |
|---|---|---|
| `MeshCore_to_ZephCore_SoftDevice_v6.{uf2,zip}` | s140 **6.x** bootloader (app @ 0x26000) | RAK4631, ThinkNode M1/M3/M6, T-Echo, Heltec T114/T096/T1, GAT562, ProMicro, … |
| `MeshCore_to_ZephCore_SoftDevice_v7.{uf2,zip}` | s140 **7.x** bootloader (app @ 0x27000) | Wio Tracker L1, T1000-E, MeshTracker X1, Ikoka Nano, SenseCAP Solar, XIAO nRF52840, … |

**Pick by the bootloader's SoftDevice** (`INFO_UF2.TXT` in DFU mode says `s140 6.x.x` or `7.x.x`).
The `.zip` carries an `--sd-req` guard (v6 `0x00B6`, v7 `0x0123`), so the bootloader rejects the
wrong one; the `.uf2` has no guard.

## Use

1. Flash the matching file: BLE OTA (MeshCore `start ota`, then the `.zip` in nRF Connect), or UF2 drag & drop.
2. It runs by itself (≈ 1–10 s; open the USB serial port within 8 s to read the log). The log lists
   MeshCore's files, what was copied, and `Verified`.
3. The board reboots into the bootloader's BLE DFU mode (advertises `AdaDFU`/`DfuTarg`). Flash the
   ZephCore **repeater** `.zip` for your board over BLE.

## What is converted

MeshCore (Adafruit InternalFS, littlefs **v1**, 128 B blocks, 7×4 KB @ `0xED000`) → ZephCore
(Zephyr littlefs **v2**, 128 KB @ `0xD4000`, `/lfs/repeater/`):

| MeshCore | ZephCore | Note |
|---|---|---|
| `/_main.id` (also `/identity/_main.id`) | `_main.id` | pub‖prv, same layout. **Required**; nothing is touched without it |
| `/prefs.json` (fallback `/config.json`) | `prefs.json` | same ConfigSerializer format; ZephCore-only keys default |
| `/com_prefs` (only if no JSON) | `prefs` | pre-1.17 binary prefs, ZephCore decodes + migrates on boot |
| `/s_contacts` | `acl` | identical 136 B records, only the name differs |
| `/regions2` | `regions2` | identical |

The two volumes overlap (`0xED000–0xF4000`), so everything is read into RAM first, `storage_partition`
(NVS bonds, `0xD0000`) and `lfs_partition` are erased, `/lfs` is created and written, then **remounted
and compared byte-for-byte** before DFU is enabled. A write failure is retried 3×; do not power off
while the log says so, the identity then exists only in RAM. In that state the log repeats the private
key every 10 s as a `set prv.key <hex>` line: copy it, and after flashing ZephCore paste it on the USB
console to restore the identity. It is printed only on this failure path.

If no MeshCore identity is found, **nothing is erased**; the board drops to UF2 DFU so you can reflash.
The same happens on a MeshCore **companion** (any of `/contacts3`, `/channels2`, `/adv_blobs`,
`/new_prefs`, or the companion key `"man_add"` in `prefs.json`): its contacts and channels have no
place in a repeater volume. A room server migrates like a repeater (identity, prefs, ACL; MeshCore
keeps its posts in RAM only); flash ZephCore's room-server `.zip` afterwards.
If a ZephCore repeater volume is already present it just enables DFU.

## Build

```
./build.sh        # both -> out/ (needs ~/Documents/ZephCore west workspace + Zephyr SDK)
./build.sh 7      # one
```

Both images are built on `nrf52840dk/nrf52840`, a base board without a `board.c`. The overlays turn
off what its devicetree would do on the board the image lands on: its UARTs (driven TX pins), NFC,
DC/DC (LDO only) and its `gpio-as-nreset` UICR write. Don't build on `rak4631`: its `board.c` sets UICR REGOUT0 (the GPIO/VDD rail) to 3.3 V on
any board powered through VDDH, and that stays until a full chip erase.

`tests/host_lfs1_test.c` checks the vendored littlefs-v1 reader on the real MeshCore geometry
(`tests/qemu` runs it on qemu_cortex_m3). `src/lfs1/` is the Adafruit littlefs v1.7 copy, symbols
renamed `lfs_`→`lfs1_` so it coexists with Zephyr's v2, malloc mapped to libc (BSD-3, see LICENSE).

## Tested

- **SoftDevice v7:** run on real hardware (XIAO nRF52840) over MeshCore 1.18.0; the config migrated correctly.
- **SoftDevice v6:** built and flash-map identical in structure, but **not run on hardware**. It is expected to
  work the same (only the app start, 0x26000 vs 0x27000, differs), but that is unconfirmed.
- The littlefs v1 reader is also checked under QEMU (`tests/`).
- The DFU hand-off (`GPREGRET=0xA8`) is the same mechanism as ZephCore's `zephcore_ble_dfu_request()`.
