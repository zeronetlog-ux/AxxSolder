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

/* True when the controller answered and a USB-C cable is attached. Set by
 * power_source_init(); the contract may still be unapplied at that point (it is
 * negotiated after the boot chooser), so this - not pd_status - tells the UI
 * that the USB-PD window belongs on screen. */
extern bool pd_cable_attached;

/* One fixed-supply mode advertised by the USB-PD source (voltage + its current
 * rating). The list is read from the source itself: a soft reset makes it repeat
 * Source_Capabilities and the STUSB4500 keeps that message in its RX buffer (see
 * stusb_read_source_pdos()). When the message cannot be read, pd_offers[] stays
 * empty and the station runs on the chip's NVM contract (PDO1, 5 V). */
/* Multiple USB-PD profiles the source advertises are listed in pd_offers[]. */
typedef struct {
    uint16_t mv;         /* PDO voltage, mV                           */
    uint16_t ma;         /* PDO current rating, mA                    */
    bool     accepted;   /* always true for entries in pd_offers[]     */
} pd_offer_t;

/* Why the last profile switch did not take effect (see request_offer() in
 * power_source.c). Surfaced to the user as a short popup. */
typedef enum {
    PD_SWITCH_OK = 0,
    PD_SWITCH_I2C,       /* the controller did not answer                     */
    PD_SWITCH_REJECTED,  /* the source sent Reject                            */
    PD_SWITCH_TIMEOUT,   /* no Accept/PS_RDY within the window                */
    PD_SWITCH_NO_VBUS    /* exchange looked fine, but VBUS stayed at 5 V      */
} pd_switch_result_t;

extern pd_switch_result_t pd_last_switch;

/* Short text for pd_last_switch, for show_popup(). */
const char *power_source_switch_error(void);

#define PD_OFFER_MAX 8
#define PD_OFFER_NONE 0xFF   /* pd_offer_selected: no profile in use (5 V NVM contract) */

/* Supply grid discovered by power_source_init(), highest power first. */
extern pd_offer_t pd_offers[PD_OFFER_MAX];
extern uint8_t    pd_offer_count;

/* Index into pd_offers[] of the profile the station currently runs on, or
 * PD_OFFER_NONE when the source accepted none of them (5 V baseline). */
extern uint8_t    pd_offer_selected;

/* Index into pd_offers[] applied when the boot chooser closes without a
 * selection: the stored preference (flash_values.pd_profile) when the source
 * still offers it, otherwise the most powerful mode. PD_OFFER_NONE when the
 * source offered nothing. The contract is negotiated from main() after the
 * chooser, so the window does not wait for the PD exchange. */
extern uint8_t    pd_offer_default;

/* Switch the link to one of the probed profiles (index into pd_offers[]) and
 * recompute the heater power limit. Returns false when the source refuses it
 * again. Used by the boot-time mode chooser. */
bool power_source_apply_offer(uint8_t index);

/* No advertised modes: adopt the contract the controller negotiated on its own
 * from its NVM settings, so its power limit and the displayed source state are
 * used. Called after the chooser, so the window does not wait for it. */
void power_source_adopt_contract(void);

/* Boot-time power-source detection. Talks to the STUSB4500 over I2C, waits for
 * the sink to become ready and derives the heater power limit from the contract
 * the controller negotiated on its own.
 *
 * The source's own capability list is read - and a better contract negotiated -
 * only when the link already runs above 5 V. At 5 V the source is a plain 5 V
 * supply or a laptop port, and those restart when the link is touched during
 * boot, so nothing is asked there and pd_offers[] stays empty. When the list is
 * asked for but cannot be read, the contract the chip has is kept and pd_status
 * becomes PD_STATUS_NO_CAPS.
 *
 * The modes that are on offer are left in pd_offers[]/pd_offer_count for the boot
 * banner.
 *
 * Blocking, but short: ~0.5 s.
 *
 * Call once on boot, after the I2C peripheral is ready. */
void power_source_init(void);

#endif /* POWER_SOURCE_H */
