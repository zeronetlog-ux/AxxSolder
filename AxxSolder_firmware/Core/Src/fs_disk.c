#include "fs_disk.h"
#include "storage.h"
#include "ff.h"
#include <string.h>

/* The FAT volume lives in the "FS" linker region - see STM32G431CBTX_FLASH.ld.
 * It must not overlap the PROFILES / SETTINGS pages (they are protected by the
 * settings and tip-profile code, which erase those pages on save). */

extern uint32_t _sfs;
extern uint32_t _efs;

#define FS_BASE         ((uintptr_t)&_sfs)
#define FS_SECTORS_TOTAL (((uintptr_t)&_efs - FS_BASE) / FS_DISK_SECTOR_SIZE)

/* Host-side unit test hook (tools/fs_test): allows running this disk layer
 * against a RAM buffer instead of the MCU flash. */
#ifndef FS_DISK_BASE
#define FS_DISK_BASE    FS_BASE
#endif
#ifndef FS_DISK_SECTORS
#define FS_DISK_SECTORS FS_SECTORS_TOTAL
#endif

#define FS_SECTORS_PER_PAGE (FS_DISK_PAGE_SIZE / FS_DISK_SECTOR_SIZE)

static uint8_t  page_cache[FS_DISK_PAGE_SIZE];
static uintptr_t page_cache_addr;      /* flash address currently cached, 0 = none */
static bool     page_cache_dirty;

/* ------------------------------------------------------------------ */
/* Page cache                                                         */
/* ------------------------------------------------------------------ */

static DRESULT page_flush(void)
{
	if (!page_cache_dirty) {
		return RES_OK;
	}

	if (!storage_flash_write_page(page_cache_addr, page_cache)) {
		return RES_ERROR;   /* keep the dirty flag: retry on the next sync */
	}

	page_cache_dirty = false;
	return RES_OK;
}

static DRESULT page_load(uintptr_t addr)
{
	if (page_cache_addr == addr) {
		return RES_OK;
	}

	/* Leaving the current page: its pending changes must reach flash first. */
	if (page_flush() != RES_OK) {
		return RES_ERROR;
	}

	memcpy(page_cache, (const void *)addr, FS_DISK_PAGE_SIZE);
	page_cache_addr  = addr;
	page_cache_dirty = false;
	return RES_OK;
}

/* ------------------------------------------------------------------ */
/* Raw sector access (also used by the USB MSC class)                 */
/* ------------------------------------------------------------------ */

uint32_t fs_disk_sector_count(void)
{
	return FS_DISK_SECTORS;
}

DRESULT fs_disk_read(BYTE *buf, uint32_t sector, uint16_t count)
{
	if ((buf == NULL) || (sector + count > FS_DISK_SECTORS)) {
		return RES_PARERR;
	}

	while (count--) {
		uintptr_t addr = FS_DISK_BASE + (sector / FS_SECTORS_PER_PAGE) * FS_DISK_PAGE_SIZE;

		if (page_load(addr) != RES_OK) {
			return RES_ERROR;
		}
		memcpy(buf, &page_cache[(sector % FS_SECTORS_PER_PAGE) * FS_DISK_SECTOR_SIZE],
		       FS_DISK_SECTOR_SIZE);
		buf += FS_DISK_SECTOR_SIZE;
		sector++;
	}

	return RES_OK;
}

DRESULT fs_disk_write(const BYTE *buf, uint32_t sector, uint16_t count)
{
	if ((buf == NULL) || (sector + count > FS_DISK_SECTORS)) {
		return RES_PARERR;
	}

	while (count--) {
		uintptr_t addr = FS_DISK_BASE + (sector / FS_SECTORS_PER_PAGE) * FS_DISK_PAGE_SIZE;

		if (page_load(addr) != RES_OK) {
			return RES_ERROR;
		}
		memcpy(&page_cache[(sector % FS_SECTORS_PER_PAGE) * FS_DISK_SECTOR_SIZE],
		       buf, FS_DISK_SECTOR_SIZE);
		page_cache_dirty = true;
		buf += FS_DISK_SECTOR_SIZE;
		sector++;
	}

	return RES_OK;
}

DRESULT fs_disk_sync(void)
{
	return page_flush();
}

/* Lay down a fresh FAT12 volume.
 *
 * f_mkfs() cannot be used here: it refuses volumes smaller than MIN_VOLUME
 * (64 sectors = 32 KB), which does not fit next to the firmware. FatFS itself
 * only needs more than 32 clusters when mounting, so this writes the VBR, FAT
 * and root directory by hand for a 40-sector volume (512-byte clusters,
 * 1 reserved sector, 1 FAT, 16 root entries -> 37 clusters).
 *
 * The volume is a "superfloppy" (no MBR), which Windows/macOS/Linux all mount
 * for FAT12. */
FRESULT fs_disk_format(void)
{
	static uint8_t sector[FS_DISK_SECTOR_SIZE];

	const uint16_t reserved     = 1;
	const uint16_t fat_sectors  = 1;
	const uint16_t root_entries = 16;
	const uint16_t root_sectors = (root_entries * 32u) / FS_DISK_SECTOR_SIZE;
	const uint16_t total        = (uint16_t)FS_DISK_SECTORS;
	const uint8_t  media        = 0xF8;

	/* Nothing cached may survive a reformat. */
	page_cache_addr  = 0;
	page_cache_dirty = false;

	if ((total <= reserved + fat_sectors + root_sectors + 32u) ||
	    (total < (FS_DISK_PAGE_SIZE / FS_DISK_SECTOR_SIZE))) {
		return FR_INVALID_PARAMETER;   /* volume too small for FAT12 */
	}

	/* ---- Boot sector (VBR) ---- */
	memset(sector, 0, sizeof(sector));
	sector[0] = 0xEB; sector[1] = 0x3C; sector[2] = 0x90;      /* jmp + nop */
	memcpy(&sector[3], "MSWIN4.1", 8);                          /* OEM name */
	sector[11] = (uint8_t)(FS_DISK_SECTOR_SIZE & 0xFF);
	sector[12] = (uint8_t)(FS_DISK_SECTOR_SIZE >> 8);           /* bytes/sector */
	sector[13] = 1;                                             /* sectors/cluster */
	sector[14] = (uint8_t)(reserved & 0xFF);
	sector[15] = (uint8_t)(reserved >> 8);                      /* reserved sectors */
	sector[16] = 1;                                             /* number of FATs */
	sector[17] = (uint8_t)(root_entries & 0xFF);
	sector[18] = (uint8_t)(root_entries >> 8);                  /* root entries */
	sector[19] = (uint8_t)(total & 0xFF);
	sector[20] = (uint8_t)(total >> 8);                         /* total sectors */
	sector[21] = media;
	sector[22] = (uint8_t)(fat_sectors & 0xFF);
	sector[23] = (uint8_t)(fat_sectors >> 8);                   /* sectors per FAT */
	sector[24] = 32; sector[26] = 2;                            /* geometry (dummy) */
	sector[36] = 0x80;                                          /* drive number */
	sector[38] = 0x29;                                          /* extended boot sig */
	sector[39] = 0x55; sector[40] = 0xB6;                       /* volume id */
	sector[41] = 0x11; sector[42] = 0x42;
	memcpy(&sector[43], "AXXSOLDER  ", 11);                     /* volume label */
	memcpy(&sector[54], "FAT12   ", 8);                         /* fs type */
	sector[62] = 0xF4;                                          /* hlt */
	sector[510] = 0x55; sector[511] = 0xAA;

	if (fs_disk_write(sector, 0, 1) != RES_OK) {
		return FR_DISK_ERR;
	}

	/* ---- FAT: entry0 = 0xFF8 (media), entry1 = 0xFFF (EOC) ---- */
	memset(sector, 0, sizeof(sector));
	sector[0] = media;
	sector[1] = 0xFF;
	sector[2] = 0xFF;

	for (uint16_t i = 0; i < fat_sectors; i++) {
		if (fs_disk_write(sector, (uint32_t)(reserved + i), 1) != RES_OK) {
			return FR_DISK_ERR;
		}
	}

	/* ---- Empty root directory ---- */
	memset(sector, 0, sizeof(sector));

	for (uint16_t i = 0; i < root_sectors; i++) {
		if (fs_disk_write(sector, (uint32_t)(reserved + fat_sectors + i), 1) != RES_OK) {
			return FR_DISK_ERR;
		}
	}

	/* Data area stays erased (0xFF): FAT entries mark every cluster as free. */
	return (fs_disk_sync() == RES_OK) ? FR_OK : FR_DISK_ERR;
}

/* ------------------------------------------------------------------ */
/* FatFS disk interface                                               */
/* ------------------------------------------------------------------ */

DSTATUS disk_status(BYTE pdrv)
{
	return (pdrv == 0) ? 0 : STA_NOINIT;
}

DSTATUS disk_initialize(BYTE pdrv)
{
	if (pdrv != 0) {
		return STA_NOINIT;
	}

	page_cache_addr  = 0;
	page_cache_dirty = false;
	return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (pdrv != 0) {
		return RES_PARERR;
	}
	return fs_disk_read(buff, (uint32_t)sector, (uint16_t)count);
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	if (pdrv != 0) {
		return RES_PARERR;
	}
	return fs_disk_write(buff, (uint32_t)sector, (uint16_t)count);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	if (pdrv != 0) {
		return RES_PARERR;
	}

	switch (cmd) {
	case CTRL_SYNC:
		return fs_disk_sync();

	case GET_SECTOR_COUNT:
		*(LBA_t *)buff = FS_DISK_SECTORS;
		return RES_OK;

	case GET_SECTOR_SIZE:
		*(WORD *)buff = FS_DISK_SECTOR_SIZE;
		return RES_OK;

	case GET_BLOCK_SIZE:
		*(DWORD *)buff = FS_SECTORS_PER_PAGE;   /* erase block = one flash page */
		return RES_OK;

	case CTRL_TRIM:
		/* Nothing to do: an erased page cannot be restored without a rewrite. */
		return RES_OK;

	default:
		return RES_PARERR;
	}
}
