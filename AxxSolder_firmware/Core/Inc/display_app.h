#ifndef DISPLAY_APP_H
#define DISPLAY_APP_H

/* High-level display orchestration: main-screen render, popup display,
 * standby/sleep countdown, deg C/F conversion, etc.
 *
 * Sits between the application code (sensors, state machine, etc.) and
 * the low-level LCD primitives in Drivers/LCD + graph.c + UGUI. */

/* Display tick interval (ms). Public so graph.c can compute the
 * time-per-pixel value on the X axis. */
#define DISPLAY_TICK_INTERVAL_MS 15

#include <stdint.h>
#include "moving_average.h"
#include "stusb4500.h"   /* stusb_dpm_trace_t (PD contract screen) */

/* Power-bar moving-average filter. File-scope so main.c initializes
 * it at boot. */
extern FilterTypeDef requested_power_filtered_filter_struct;

/* "SLEEP/STANDBY label has been drawn" flags. Cleared by callers
 * (e.g. buttons.c after a preset save, show_popup) so the next display
 * tick redraws the overlay. */
extern uint8_t sleep_state_written_to_LCD;
extern uint8_t standby_state_written_to_LCD;

/* Per-main-loop dispatcher:
 *   - If a popup is showing and its dwell time has elapsed, dismiss it
 *     and redraw the main screen.
 *   - Otherwise, run the periodic main-screen / graph-view update at
 *     ~67 Hz (15 ms rate-limit).
 *
 * Self-gated; safe to call every loop iteration. */
void display_app_tick(void);

/* Draw the static main-screen layout (labels, frames, preset readouts).
 * Called on boot, after a popup dismisses, and after a settings save. */
void LCD_draw_main_screen(void);

/* Show a transient orange popup with the given text. Auto-dismisses
 * after the popup interval. */
void show_popup(char *text);

/* Show the ground-fault popup and halt (calls Error_Handler()).
 * Called from the ADC watchdog ISR via main.c's dispatcher. */
void LCD_draw_earth_fault_popup(void);

/* Boot-time USB-PD mode chooser: lists the modes the source advertises (read
 * from its Source_Capabilities by power_source_init()), lets the user pick one
 * with the rotary encoder and confirms with any button. Returns the chosen index
 * into pd_offers[], or -1 when the source offered nothing. Called once from
 * main() right after power_source_init(). */
int8_t LCD_run_pd_banner(void);

/* Diagnostic screens for the USB-PD path, compiled into STUSB_DEBUG builds
 * (tools/build.sh --debug) only.
 *
 *   LCD_show_pd_apply(): what was requested, the measured voltage, the raw RDO
 *       word with its decoded fields and the resulting power limit
 *   LCD_show_nvm_dump(): the controller's own NVM settings
 */
#ifdef STUSB_DEBUG
void LCD_show_pd_apply(float req_v, float req_a, float vbus, uint32_t rdo_raw,
                       uint8_t rdo_pos, float rdo_max_a, float rdo_oper_a,
                       float contract, float limit, const stusb_dpm_trace_t *trace);
void LCD_show_nvm_dump(const uint8_t *nvm);
#endif

/* Idle time after which the chooser closes on its own (ms). Any encoder
 * movement or button press restarts the countdown; only a button press changes
 * the selected mode. */
#define PD_BANNER_TIMEOUT_MS 20000

/* Convert a Celsius temperature to the user's preferred unit
 * (deg C or deg F per flash_values.deg_celsius). */
float convert_temperature(float temperature);

#endif /* DISPLAY_APP_H */
