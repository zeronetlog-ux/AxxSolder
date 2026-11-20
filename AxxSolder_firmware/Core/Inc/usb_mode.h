#ifndef USB_MODE_H
#define USB_MODE_H

/* USB-storage mode: entered from main() when SW_2 is held while powering up.
 *
 * In this mode the firmware does not run the control loop at all - the heater,
 * PID, sensors and state machine stay off. Only the display, settings and the
 * USB mass-storage stack are alive. The device leaves the mode only when the
 * power is removed (like DFU mode). */
void usb_storage_mode_run(void);

/* Called from the MSC write callback (USB interrupt context). */
void usb_mode_mark_cfg_dirty(void);

/* Provided by main.c (needs the file-static MX_*_Init helpers): TIM1 is already
 * initialised there, this drives the heater outputs with zero duty. */
void app_heater_hardware_off(void);

#endif /* USB_MODE_H */
