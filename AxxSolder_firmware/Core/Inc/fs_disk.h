#ifndef INC_FS_DISK_H_
#define INC_FS_DISK_H_

#include <stdint.h>
#include <stdbool.h>
#include "diskio.h"

/* FatFS disk I/O layer for the "FS" flash region (see the linker script).
 *
 * The volume is 18 KB: 36 sectors x 512 B. One 2 KB flash page is cached so a
 * sector write becomes a read-modify-write of a whole page (flash can only be
 * erased page-wise). Used both by FatFS itself and by the USB MSC class. */

#define FS_DISK_SECTOR_SIZE   512u
#define FS_DISK_PAGE_SIZE     2048u   /* STM32G431 flash page size */

/* Number of 512-byte sectors in the volume (from the linker region). */
uint32_t fs_disk_sector_count(void);

/* Write a dirty cached page back to flash. Called on CTRL_SYNC and after each
 * MSC write burst so the host can safely unplug. */
DRESULT fs_disk_sync(void);

/* Format the volume (FAT12, 512-byte clusters, 16 root entries). */
FRESULT fs_disk_format(void);

/* Raw sector access for the USB MSC class (logical unit 0). */
DRESULT fs_disk_read(BYTE *buf, uint32_t sector, uint16_t count);
DRESULT fs_disk_write(const BYTE *buf, uint32_t sector, uint16_t count);

#endif /* INC_FS_DISK_H_ */
