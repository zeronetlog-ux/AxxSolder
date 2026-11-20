#ifndef USB_MSC_IF_H
#define USB_MSC_IF_H

#include <stdbool.h>
#include "usbd_msc.h"

/* Storage callbacks handed to the MSC class (usb_msc_if.c). */
extern USBD_StorageTypeDef USBD_MSC_fops;

/* The class reports "ready" only once the FAT volume is mounted, so the host
 * cannot touch the media while it is being formatted. */
void usb_msc_if_set_ready(bool ready);

/* Tick (HAL_GetTick) of the last host read/write, and the SCSI medium state
 * (SCSI_MEDIUM_UNLOCKED / LOCKED / EJECTED). Used to leave the mode
 * automatically once the host ejects the volume. */
uint32_t usb_msc_last_io_tick(void);
uint8_t  usb_msc_medium_state(void);

#endif /* USB_MSC_IF_H */
