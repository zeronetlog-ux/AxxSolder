#ifndef HOST_FLASH_H
#define HOST_FLASH_H

#include <stdint.h>

/* RAM buffer that stands in for the "FS" flash region during host tests. */
#define HOST_FLASH_SIZE   (40u * 512u)   /* 20 KB, matches the linker region */

extern uint8_t host_flash_region[HOST_FLASH_SIZE];

#endif /* HOST_FLASH_H */
