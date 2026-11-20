#include "usb_msc_if.h"
#include "usb_mode.h"
#include "usb_device.h"
#include "fs_disk.h"
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* SCSI INQUIRY payload (standard 36-byte response)                   */
/* ------------------------------------------------------------------ */

#define INQUIRY_DATA_LEN   36U

static const uint8_t msc_inquiry_data[INQUIRY_DATA_LEN] = {
    /* LUN 0: direct-access block device, removable media */
    0x00, 0x80, 0x02, 0x02,
    (INQUIRY_DATA_LEN - 5),         /* additional length (31) */
    0x00, 0x00, 0x00,
    /* Vendor identification (8 bytes) */
    'A', 'x', 'x', 'S', 'o', 'l', 'd', 'r',
    /* Product identification (16 bytes) */
    'U', 'S', 'B', ' ', 'S', 't', 'o', 'r',
    'a', 'g', 'e', ' ', ' ', ' ', ' ', ' ',
    /* Product revision level (4 bytes) */
    '1', '.', '0', '0'
};

static volatile bool volume_ready;
static volatile uint32_t last_io_tick;

void usb_msc_if_set_ready(bool ready)
{
    volume_ready = ready;
}

uint32_t usb_msc_last_io_tick(void)
{
    return last_io_tick;
}

uint8_t usb_msc_medium_state(void)
{
    USBD_MSC_BOT_HandleTypeDef *hmsc =
        (USBD_MSC_BOT_HandleTypeDef *)hUsbDeviceFS.pClassData;

    if (hmsc == NULL) {
        return SCSI_MEDIUM_UNLOCKED;
    }
    return hmsc->scsi_medium_state;
}

/* ------------------------------------------------------------------ */
/* USBD_MSC callbacks                                                 */
/* ------------------------------------------------------------------ */

static int8_t msc_init(uint8_t lun)
{
    (void)lun;
    return volume_ready ? 0 : -1;
}

static int8_t msc_get_capacity(uint8_t lun, uint32_t *block_num, uint16_t *block_size)
{
    (void)lun;

    *block_num  = fs_disk_sector_count();
    *block_size = FS_DISK_SECTOR_SIZE;
    return 0;
}

static int8_t msc_is_ready(uint8_t lun)
{
    (void)lun;
    return volume_ready ? 0 : -1;
}

static int8_t msc_is_write_protected(uint8_t lun)
{
    (void)lun;
    return 0;   /* the config file is meant to be edited from the host */
}

static int8_t msc_read(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
    (void)lun;
    last_io_tick = HAL_GetTick();

    if (fs_disk_read(buf, blk_addr, blk_len) != RES_OK) {
        return -1;
    }
    return 0;
}

static int8_t msc_write(uint8_t lun, uint8_t *buf, uint32_t blk_addr, uint16_t blk_len)
{
    (void)lun;
    last_io_tick = HAL_GetTick();
    if (fs_disk_write(buf, blk_addr, blk_len) != RES_OK) {
        return -1;
    }

    /* Push the dirty page out now: the user may cut power at any moment. */
    (void)fs_disk_sync();

    /* Runs in the USB interrupt: only set a flag, parsing happens in the
     * main loop once the host has been quiet for a while. */
    usb_mode_mark_cfg_dirty();
    return 0;
}

static int8_t msc_get_max_lun(void)
{
    return 0;
}

USBD_StorageTypeDef USBD_MSC_fops = {
    msc_init,
    msc_get_capacity,
    msc_is_ready,
    msc_is_write_protected,
    msc_read,
    msc_write,
    msc_get_max_lun,
    (int8_t *)msc_inquiry_data,
};
