#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "usbd_def.h"
#include "stm32g4xx_hal.h"

/* Handles of the USB-device instance used by the USB-storage mode.
 * Defined in usbd_conf.c, initialised in usb_mode.c. */

extern USBD_HandleTypeDef hUsbDeviceFS;
extern PCD_HandleTypeDef  hpcd_USB_FS;

#endif /* USB_DEVICE_H */
