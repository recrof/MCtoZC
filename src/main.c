/*
 * SPDX-License-Identifier: MIT
 *
 * MeshCore -> ZephCore repeater migrator (nRF52840, Adafruit bootloader)
 *
 * Flashed over a running MeshCore repeater (UF2 or BLE OTA zip). MeshCore keeps its
 * settings in the Adafruit core's InternalFS: littlefs v1, 128-byte blocks, 7 x 4 KB
 * at 0xED000. ZephCore keeps them in Zephyr's littlefs v2 volume at 0xD4000..0xF4000,
 * under /lfs/repeater/. The two overlap (0xED000..0xF4000), so the order is fixed:
 *
 *   1. read MeshCore's volume (read-only littlefs v1) into RAM, and validate it
 *   2. erase ZephCore's NVS + LittleFS partitions (this also destroys MeshCore's volume)
 *   3. format /lfs, write /lfs/repeater/{_main.id,prefs.json,acl,regions2}
 *   4. unmount, remount, read every file back and compare with RAM
 *   5. reboot into the bootloader's BLE OTA DFU mode, ready for the ZephCore zip
 *
 * Nothing is erased unless step 1 produced an identity. If it did not, flash is left
 * as found and the node drops into UF2 DFU so MeshCore can be flashed back.
 *
 * File formats need no conversion: ZephCore's prefs.json is MeshCore's ConfigSerializer
 * format (unknown/missing keys keep defaults), _main.id is pub||prv in both, and the
 * ACL and region files have identical records; only /s_contacts is named /acl there.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <string.h>
#include <errno.h>

#include <hal/nrf_power.h>

#include "lfs1.h"

/* Adafruit bootloader GPREGRET commands */
#define DFU_MAGIC_OTA_RESET   0xA8  /* BLE OTA DFU ("AdaDFU" advertising) */
#define DFU_MAGIC_UF2_RESET   0x57  /* USB mass-storage DFU */

/* MeshCore / Adafruit InternalFS geometry (nRF52840). */
#define MC_LFS_ADDR           0xED000u
#define MC_LFS_BLOCK_SIZE     128u
#define MC_LFS_BLOCK_COUNT    ((7u * 4096u) / MC_LFS_BLOCK_SIZE)

#define ZC_DIR                "/lfs/repeater"

#define ID_LEN_FULL           96   /* pub(32) || prv(64), what both firmwares write */
#define ID_LEN_PRV            64   /* private key only; ZephCore derives the public key */

BUILD_ASSERT(PARTITION_EXISTS(lfs_partition) && PARTITION_EXISTS(storage_partition),
	     "overlay must define ZephCore's lfs_partition and storage_partition");

/* ZephCore's /lfs entry from the devicetree (same parameters as ZephCore's build). */
FS_FSTAB_DECLARE_ENTRY(DT_NODELABEL(lfs));
#define ZC_MP (&FS_FSTAB_ENTRY(DT_NODELABEL(lfs)))

/* ── MeshCore volume snapshot ─────────────────────────────────── */

struct item {
	const char *src[3];      /* MeshCore paths, first hit wins */
	const char *dst;         /* name under /lfs/repeater */
	uint8_t *buf;
	size_t cap;
	size_t len;
	bool found;
	bool required;
};

static uint8_t buf_id[128];
static uint8_t buf_prefs[8192];
static uint8_t buf_legacy[512];
static uint8_t buf_acl[16384];
static uint8_t buf_regions[16384];

enum { IT_ID, IT_PREFS, IT_LEGACY, IT_ACL, IT_REGIONS, IT_COUNT };

static struct item items[IT_COUNT] = {
	[IT_ID] = { { "/_main.id", "/identity/_main.id" }, "_main.id",
		    buf_id, sizeof(buf_id), 0, false, true },
	/* "config.json" is accepted as an alias in case a build named it that way. */
	[IT_PREFS] = { { "/prefs.json", "/config.json" }, "prefs.json",
		       buf_prefs, sizeof(buf_prefs), 0, false, false },
	/* Pre-JSON MeshCore (< 1.17) binary prefs; ZephCore decodes them as /lfs/repeater/prefs. */
	[IT_LEGACY] = { { "/com_prefs" }, "prefs",
			buf_legacy, sizeof(buf_legacy), 0, false, false },
	[IT_ACL] = { { "/s_contacts" }, "acl",
		     buf_acl, sizeof(buf_acl), 0, false, false },
	[IT_REGIONS] = { { "/regions2" }, "regions2",
			 buf_regions, sizeof(buf_regions), 0, false, false },
};

static lfs1_t mc_lfs;
static uint8_t mc_rd[MC_LFS_BLOCK_SIZE] __aligned(4);
static uint8_t mc_pg[MC_LFS_BLOCK_SIZE] __aligned(4);
static uint8_t mc_la[(128 / 8)] __aligned(4);
static uint8_t mc_fb[MC_LFS_BLOCK_SIZE] __aligned(4);

static int mc_read(const struct lfs1_config *c, lfs1_block_t block, lfs1_off_t off,
		   void *buffer, lfs1_size_t size)
{
	ARG_UNUSED(c);
	/* Internal flash is memory-mapped; this volume is only ever read. */
	memcpy(buffer, (const void *)(MC_LFS_ADDR + block * MC_LFS_BLOCK_SIZE + off), size);
	return 0;
}

static int mc_prog(const struct lfs1_config *c, lfs1_block_t block, lfs1_off_t off,
		   const void *buffer, lfs1_size_t size)
{
	ARG_UNUSED(c); ARG_UNUSED(block); ARG_UNUSED(off); ARG_UNUSED(buffer); ARG_UNUSED(size);
	return LFS1_ERR_IO;     /* read-only: never modify the source */
}

static int mc_erase(const struct lfs1_config *c, lfs1_block_t block)
{
	ARG_UNUSED(c); ARG_UNUSED(block);
	return LFS1_ERR_IO;
}

static int mc_sync(const struct lfs1_config *c)
{
	ARG_UNUSED(c);
	return 0;
}

static const struct lfs1_config mc_cfg = {
	.read = mc_read, .prog = mc_prog, .erase = mc_erase, .sync = mc_sync,
	.read_size = MC_LFS_BLOCK_SIZE, .prog_size = MC_LFS_BLOCK_SIZE,
	.block_size = MC_LFS_BLOCK_SIZE, .block_count = MC_LFS_BLOCK_COUNT,
	.lookahead = 128,
	.read_buffer = mc_rd, .prog_buffer = mc_pg,
	.lookahead_buffer = mc_la, .file_buffer = mc_fb,
};

static void list_dir(const char *path)
{
	lfs1_dir_t d;
	struct lfs1_info info;

	if (lfs1_dir_open(&mc_lfs, &d, path) < 0) {
		return;
	}
	while (lfs1_dir_read(&mc_lfs, &d, &info) > 0) {
		if (!strcmp(info.name, ".") || !strcmp(info.name, "..")) {
			continue;
		}
		if (info.type == LFS1_TYPE_DIR) {
			printk("    %s%s/\n", strcmp(path, "/") ? path : "", info.name);
		} else {
			printk("    %s%s  (%u bytes)\n", strcmp(path, "/") ? path : "", info.name,
			       (unsigned)info.size);
		}
	}
	lfs1_dir_close(&mc_lfs, &d);
}

/* 0 = ok (found or absent), <0 = present but unreadable / too big for its buffer */
static int read_item(struct item *it)
{
	for (size_t i = 0; i < ARRAY_SIZE(it->src) && it->src[i]; i++) {
		struct lfs1_info info;

		if (lfs1_stat(&mc_lfs, it->src[i], &info) < 0 || info.type != LFS1_TYPE_REG) {
			continue;
		}
		if (info.size > it->cap) {
			printk("  %s: %u bytes does not fit (%u) - refusing to truncate\n",
			       it->src[i], (unsigned)info.size, (unsigned)it->cap);
			return -EFBIG;
		}

		lfs1_file_t f;

		if (lfs1_file_open(&mc_lfs, &f, it->src[i], LFS1_O_RDONLY) < 0) {
			printk("  %s: open failed\n", it->src[i]);
			return -EIO;
		}
		lfs1_ssize_t n = lfs1_file_read(&mc_lfs, &f, it->buf, info.size);

		lfs1_file_close(&mc_lfs, &f);
		if (n < 0 || (lfs1_size_t)n != info.size) {
			printk("  %s: read failed (%d)\n", it->src[i], (int)n);
			return -EIO;
		}
		it->len = info.size;
		it->found = true;
		printk("  %s -> %s/%s  (%u bytes)\n", it->src[i], ZC_DIR, it->dst,
		       (unsigned)it->len);
		return 0;
	}
	return 0;
}

static bool blank_or_zero(const uint8_t *p, size_t n)
{
	uint8_t a = 0, b = 0xFF;

	for (size_t i = 0; i < n; i++) {
		a |= p[i];
		b &= p[i];
	}
	return a == 0 || b == 0xFF;
}

/*
 * A MeshCore companion also keeps /_main.id and /prefs.json at the root, so
 * those do not tell it apart. Its own files do, while they are on this volume;
 * on boards with QSPI flash it moves contacts, channels and blobs there, and
 * then only its prefs.json key "man_add" (manual contact add) remains.
 */
static const char *const companion_files[] = {
	"/contacts3", "/channels2", "/adv_blobs", "/new_prefs",
};

static bool companion_file_present(void)
{
	struct lfs1_info info;

	for (size_t i = 0; i < ARRAY_SIZE(companion_files); i++) {
		if (lfs1_stat(&mc_lfs, companion_files[i], &info) >= 0) {
			printk("  %s is a MeshCore companion file.\n", companion_files[i]);
			return true;
		}
	}
	return false;
}

static bool contains(const uint8_t *buf, size_t len, const char *s)
{
	size_t n = strlen(s);

	for (size_t i = 0; i + n <= len; i++) {
		if (memcmp(buf + i, s, n) == 0) {
			return true;
		}
	}
	return false;
}

/* Step 1. Returns 0 when a usable MeshCore repeater volume was read into RAM. */
static int snapshot_meshcore(void)
{
	int rc = lfs1_mount(&mc_lfs, &mc_cfg);

	if (rc < 0) {
		printk("MeshCore volume at 0x%X: not a littlefs v1 volume (%d)\n", MC_LFS_ADDR, rc);
		return -ENODEV;
	}
	printk("MeshCore volume at 0x%X mounted (littlefs v1). Contents:\n", MC_LFS_ADDR);
	list_dir("/");
	list_dir("/identity");

	int err = 0;
	bool companion = companion_file_present();

	/* The legacy binary prefs only matter when there is no prefs.json. */
	for (int i = 0; i < IT_COUNT && !err && !companion; i++) {
		if (i == IT_LEGACY && items[IT_PREFS].found) {
			continue;
		}
		err = read_item(&items[i]);
	}
	lfs1_unmount(&mc_lfs);

	if (err) {
		return err;
	}
	if (!companion && items[IT_PREFS].found &&
	    contains(items[IT_PREFS].buf, items[IT_PREFS].len, "\"man_add\"")) {
		printk("  prefs.json has the companion key \"man_add\".\n");
		companion = true;
	}
	if (companion) {
		printk("This is a MeshCore companion, not a repeater or room server: its contacts "
		       "and channels would be lost.\n");
		return -ENOTSUP;
	}
	struct item *id = &items[IT_ID];

	if (!id->found) {
		printk("No /_main.id: this is not a MeshCore repeater volume.\n");
		return -ENOENT;
	}
	if ((id->len != ID_LEN_FULL && id->len != ID_LEN_PRV) || blank_or_zero(id->buf, id->len)) {
		printk("/_main.id is %u bytes / blank: not a valid identity.\n", (unsigned)id->len);
		return -EINVAL;
	}
	if (!items[IT_PREFS].found && !items[IT_LEGACY].found) {
		printk("WARNING: no prefs.json (or config.json) found; ZephCore will start with "
		       "default settings and keep the identity.\n");
	} else if (items[IT_PREFS].found) {
		const uint8_t *p = items[IT_PREFS].buf;
		size_t i = 0;

		while (i < items[IT_PREFS].len && (p[i] == ' ' || p[i] == '\n' || p[i] == '\r' ||
						   p[i] == '\t')) {
			i++;
		}
		if (i >= items[IT_PREFS].len || p[i] != '{') {
			printk("WARNING: prefs.json does not look like JSON; copying it anyway "
			       "(ZephCore falls back to defaults if it cannot parse it).\n");
		}
	}
	return 0;
}

/* ── ZephCore volume ──────────────────────────────────────────── */

static int erase_partition(uint8_t id, const char *name)
{
	const struct flash_area *fa;
	int rc = flash_area_open(id, &fa);

	if (rc) {
		printk("  %s: open failed (%d)\n", name, rc);
		return rc;
	}
	printk("  %s: erasing 0x%lx - 0x%lx\n", name, (unsigned long)fa->fa_off,
	       (unsigned long)(fa->fa_off + fa->fa_size));
	rc = flash_area_erase(fa, 0, fa->fa_size);
	flash_area_close(fa);
	if (rc) {
		printk("  %s: erase FAILED (%d)\n", name, rc);
	}
	return rc;
}

static int zc_mount(bool allow_format)
{
	ZC_MP->flags = allow_format ? 0 : FS_MOUNT_FLAG_NO_FORMAT;
	return fs_mount(ZC_MP);
}

static int put_file(const char *name, const uint8_t *buf, size_t len)
{
	char path[48];
	struct fs_file_t f;

	snprintf(path, sizeof(path), "%s/%s", ZC_DIR, name);
	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);

	if (rc) {
		printk("  %s: create failed (%d)\n", path, rc);
		return rc;
	}
	ssize_t n = fs_write(&f, buf, len);

	if (n < 0 || (size_t)n != len) {
		printk("  %s: write failed (%d)\n", path, (int)n);
		fs_close(&f);
		return n < 0 ? (int)n : -EIO;
	}
	rc = fs_sync(&f);
	fs_close(&f);
	if (rc) {
		printk("  %s: sync failed (%d)\n", path, rc);
	}
	return rc;
}

static int check_file(const char *name, const uint8_t *want, size_t len)
{
	static uint8_t chunk[256];
	char path[48];
	struct fs_file_t f;
	struct fs_dirent st;

	snprintf(path, sizeof(path), "%s/%s", ZC_DIR, name);
	if (fs_stat(path, &st) || st.size != len) {
		printk("  %s: missing or wrong size\n", path);
		return -EIO;
	}
	fs_file_t_init(&f);
	if (fs_open(&f, path, FS_O_READ)) {
		return -EIO;
	}
	size_t off = 0;
	int rc = 0;

	while (off < len) {
		size_t n = MIN(sizeof(chunk), len - off);

		if (fs_read(&f, chunk, n) != (ssize_t)n || memcmp(chunk, want + off, n) != 0) {
			printk("  %s: content mismatch at %u\n", path, (unsigned)off);
			rc = -EIO;
			break;
		}
		off += n;
	}
	fs_close(&f);
	return rc;
}

static int write_zephcore_volume(void)
{
	int rc = erase_partition(PARTITION_ID(storage_partition), "NVS (storage_partition)");

	rc = rc ?: erase_partition(PARTITION_ID(lfs_partition), "LittleFS (lfs_partition)");
	if (rc) {
		return rc;
	}

	rc = zc_mount(true);    /* blank flash: Zephyr formats, then mounts */
	if (rc) {
		printk("  /lfs: format+mount failed (%d)\n", rc);
		return rc;
	}
	rc = fs_mkdir(ZC_DIR);
	if (rc && rc != -EEXIST) {
		printk("  %s: mkdir failed (%d)\n", ZC_DIR, rc);
		fs_unmount(ZC_MP);
		return rc;
	}
	/* Identity first: it is the one file that cannot be regenerated. */
	for (int i = 0; i < IT_COUNT && !rc; i++) {
		if (items[i].found) {
			rc = put_file(items[i].dst, items[i].buf, items[i].len);
		}
	}
	fs_unmount(ZC_MP);
	if (rc) {
		return rc;
	}

	/* Read everything back from a fresh mount, as ZephCore will see it. */
	rc = zc_mount(false);
	if (rc) {
		printk("  /lfs: remount failed (%d)\n", rc);
		return rc;
	}
	for (int i = 0; i < IT_COUNT && !rc; i++) {
		if (items[i].found) {
			rc = check_file(items[i].dst, items[i].buf, items[i].len);
		}
	}
	fs_unmount(ZC_MP);
	return rc;
}

/* A previous run (or ZephCore itself) already left a repeater volume. */
static bool zephcore_volume_ready(void)
{
	struct fs_dirent st;
	bool ok = false;

	if (zc_mount(false) == 0) {
		ok = fs_stat(ZC_DIR "/_main.id", &st) == 0;
		fs_unmount(ZC_MP);
	}
	return ok;
}

/* ── Reboot paths ─────────────────────────────────────────────── */

static void wait_for_terminal(void)
{
	const struct device *cdc = DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
	uint32_t dtr = 0;

	/* Up to 8 s for a serial terminal, so the log can be read; migration runs without one. */
	for (int i = 0; i < 80 && !dtr; i++) {
		uart_line_ctrl_get(cdc, UART_LINE_CTRL_DTR, &dtr);
		k_msleep(100);
	}
	k_msleep(300);
}

/*
 * Last resort when the volume could not be written: the identity is now only in
 * RAM. Print the private key in the form ZephCore's `set prv.key <hex>` takes
 * (the 64-byte expanded key; _main.id is pub(32) || prv(64), or prv alone).
 */
static void print_private_key(void)
{
	const struct item *id = &items[IT_ID];
	const uint8_t *prv = id->buf + (id->len == ID_LEN_FULL ? ID_LEN_FULL - ID_LEN_PRV : 0);

	printk("Private key (keep it secret). Flash ZephCore, then on its USB console:\n");
	printk("  set prv.key ");
	for (int i = 0; i < ID_LEN_PRV; i++) {
		printk("%02x", prv[i]);
	}
	printk("\n");
}

static void __noreturn reboot_into(uint8_t magic)
{
	nrf_power_gpregret_set(NRF_POWER, 0, magic);
	sys_reboot(SYS_REBOOT_COLD);
	CODE_UNREACHABLE;
}

int main(void)
{
	wait_for_terminal();
	printk("\n=== MeshCore -> ZephCore repeater migrator ===\n\n");

	printk("[1/4] Reading MeshCore volume\n");
	int rc = snapshot_meshcore();

	if (rc) {
		if (zephcore_volume_ready()) {
			printk("\nNo MeshCore data, but a ZephCore repeater volume is already in "
			       "place (migration done earlier).\n");
			goto dfu;
		}
		printk("\nNOTHING WAS CHANGED. Flash is exactly as it was found.\n");
		printk("Rebooting into UF2 DFU so firmware can be flashed.\n");
		k_msleep(4000);
		reboot_into(DFU_MAGIC_UF2_RESET);
	}

	/* The copy in RAM is now the only complete one; retry the write, do not abandon it. */
	printk("\n[2/4] Writing ZephCore volume\n");
	for (int attempt = 1;; attempt++) {
		rc = write_zephcore_volume();
		if (!rc) {
			break;
		}
		printk("  attempt %d failed (%d)\n", attempt, rc);
		if (attempt >= 3) {
			/* Repeated so a terminal opened late still sees the key. */
			for (;;) {
				printk("\nMIGRATION FAILED. Do not power off: the identity is only in "
				       "RAM.\n");
				printk("Reset to retry (the old volume may already be erased).\n");
				print_private_key();
				k_msleep(10000);
			}
		}
	}
	printk("[3/4] Verified: all files read back identical from a fresh mount.\n");

dfu:
	printk("[4/4] Enabling BLE DFU\n");
	printk("Rebooting into the bootloader's BLE OTA DFU mode (advertises as AdaDFU / DfuTarg).\n");
	printk("Flash the ZephCore repeater .zip over BLE now (nRF Connect / Mesh configurator).\n");
	k_msleep(3000);
	reboot_into(DFU_MAGIC_OTA_RESET);
}
