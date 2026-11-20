#ifndef USB_DEVICE_H
#define USB_DEVICE_H

#include "usbd_def.h"
#include "stm32g4xx_hal.h"

/* Handles of the USB-device instance used by the USB-storage mode.
 * Defined in usbd_conf.c, initialised in usb_mode.c. */

extern USBD_HandleTypeDef hUsbDeviceFS;
extern PCD_HandleTypeDef  hpcd_USB_FS;

/* Incremented by every USB interrupt: >0 means the host is really talking to
 * us (bus reset / SOF / setup). Shown on the USB-storage diagnostics line. */
extern volatile uint32_t usb_irq_count;

/* Per-event counters, read from USB->ISTR in the interrupt handler. */
extern volatile uint32_t usb_reset_count;   /* bus resets seen                */
extern volatile uint32_t usb_ctr_count;     /* completed transfers (CTR)      */
extern volatile uint32_t usb_err_count;     /* error events (CRC/bit-stuff)   */

#endif /* USB_DEVICE_H */
