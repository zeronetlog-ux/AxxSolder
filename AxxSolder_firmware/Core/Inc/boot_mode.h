#ifndef BOOT_MODE_H
#define BOOT_MODE_H

#include <stdbool.h>

/* Checks, right after reset, whether SW_2 (preset 1) is held down.
 *
 * Holding it while the station is powered up selects "USB storage" mode
 * instead of a normal boot: the heater stays off, the display shows a notice
 * and the settings file is published as a removable disk. The mode is left by
 * removing the power (same idea as the ROM DFU mode on SW_3/BOOT0).
 *
 * Must be called after MX_GPIO_Init(). Blocks for up to the confirmation time.
 */
bool boot_request_usb_storage(void);

/* Checks, right after reset, whether SW_1 (the encoder) is held down.
 *
 * With the "PD auto max mode" setting the boot chooser is normally skipped; this
 * gesture brings it back for that boot, so the mode can still be picked without
 * clearing the setting. Returns quickly when the button is not held.
 */
bool boot_request_pd_chooser(void);

#endif /* BOOT_MODE_H */
