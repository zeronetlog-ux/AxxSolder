#ifndef POWER_SOURCE_H
#define POWER_SOURCE_H

#include <stdint.h>
#include <stdbool.h>

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
 * as a short hint ("XT", "XT?21", "PD?C25", "PD?R") so a missing PD contract can
 * be diagnosed from the screen without a serial console. */
typedef enum {
    PD_STATUS_NO_CHIP = 0,   /* STUSB4500 not found / I2C failure      */
    PD_STATUS_NO_SINK,       /* chip found, no cable / sink attached   */
    PD_STATUS_NO_CONTRACT,   /* cable attached, but no PD contract     */
    PD_STATUS_NO_CAPS,       /* running on the 5 V NVM contract: the
                              * source's Source_Capabilities could not be
                              * read, so no high-power profile was usable */
    PD_STATUS_OK             /* PD contract negotiated                 */
} pd_status_t;

extern pd_status_t pd_status;

/* Raw REG_DEVICE_ID value from the last detection (0 when the read failed). */
extern uint8_t pd_chip_id;

/* One fixed-supply mode advertised by the USB-PD source (voltage + its current
 * rating). The list is read from the source itself: a soft reset makes it repeat
 * Source_Capabilities and the STUSB4500 keeps that message in its RX buffer (see
 * stusb_read_source_pdos()). When the message cannot be read, pd_offers[] stays
 * empty and the station runs on the chip's NVM contract (PDO1, 5 V). */
typedef struct {
    uint16_t mv;         /* PDO voltage, mV                           */
    uint16_t ma;         /* PDO current rating, mA                    */
    bool     accepted;   /* always true for entries in pd_offers[]     */
} pd_offer_t;

#define PD_OFFER_MAX 8
#define PD_OFFER_NONE 0xFF   /* pd_offer_selected: no profile in use (5 V NVM contract) */

/* Supply grid discovered by power_source_init(), highest power first. */
extern pd_offer_t pd_offers[PD_OFFER_MAX];
extern uint8_t    pd_offer_count;

/* Index into pd_offers[] of the profile the station currently runs on, or
 * PD_OFFER_NONE when the source accepted none of them (5 V baseline). */
extern uint8_t    pd_offer_selected;

/* Switch the link to one of the probed profiles (index into pd_offers[]) and
 * recompute the heater power limit. Returns false when the source refuses it
 * again. Used by the boot-time mode chooser. */
bool power_source_apply_offer(uint8_t index);

/* Boot-time power-source detection. Talks to the STUSB4500 over I2C, waits for
 * the sink to become ready, reads the source's Source_Capabilities, keeps the
 * most powerful fixed PDO (or the user's stored voltage when the source still
 * offers it), negotiates it and writes sensor_values.USB_PD_power_limit.
 * The advertised modes are left in pd_offers[]/pd_offer_count for the boot
 * banner. When the capability list cannot be read, the chip's own NVM contract
 * (5 V) is kept and pd_status becomes PD_STATUS_NO_CAPS.
 *
 * Blocking, but short: ~0.3 s on a cooperative source (the list is read, not
 * probed).
 *
 * Call once on boot, after the I2C peripheral is ready. */
void power_source_init(void);

#endif /* POWER_SOURCE_H */
