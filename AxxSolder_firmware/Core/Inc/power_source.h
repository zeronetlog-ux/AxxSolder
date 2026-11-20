#ifndef POWER_SOURCE_H
#define POWER_SOURCE_H

#include <stdint.h>

/* The power source currently feeding the iron.
 *
 * Set during boot by power_source_init() after the STUSB4500 chip
 * has been queried over I2C:
 *   - POWER_DC  : 9-24 V from the barrel jack (default)
 *   - POWER_USB : USB-PD source negotiated successfully
 *   - POWER_BAT : reserved for a future portable variant
 *
 * Read by the display code to render the "DC" / "USB" / "BAT" label
 * on the main screen. */
typedef enum {
    POWER_DC,
    POWER_USB,
    POWER_BAT
} power_source_t;

extern power_source_t power_source;

/* Why the PD detection ended where it did. Rendered next to the input voltage
 * as a short hint ("XT", "XT?21", "PD?C25") so a missing PD contract can be
 * diagnosed from the screen without a serial console. */
typedef enum {
    PD_STATUS_NO_CHIP = 0,   /* STUSB4500 not found / I2C failure      */
    PD_STATUS_NO_SINK,       /* chip found, no cable / sink attached   */
    PD_STATUS_NO_CONTRACT,   /* cable attached, but no PD contract     */
    PD_STATUS_OK             /* PD contract negotiated                 */
} pd_status_t;

extern pd_status_t pd_status;

/* Raw REG_DEVICE_ID value from the last detection (0 when the read failed). */
extern uint8_t pd_chip_id;

/* Boot-time power-source detection. Talks to the STUSB4500 over I2C,
 * waits for the sink to become ready, asks for the available PDOs,
 * picks the highest-power one, and writes sensor_values.USB_PD_power_limit.
 *
 * Blocking. May take up to ~3.5 seconds in the worst case (3 s sink
 * timeout + 500 ms wait + ~100 ms for PDO read).
 *
 * Call once on boot, after the I2C peripheral is ready. */
void power_source_init(void);

#endif /* POWER_SOURCE_H */
