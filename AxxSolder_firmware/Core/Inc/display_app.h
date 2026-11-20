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

/* Boot-time USB-PD mode chooser: lists the modes discovered by
 * power_source_init(), lets the user pick one with the rotary encoder and
 * confirms with any button. Returns the chosen index into pd_offers[], or -1
 * when the source offered nothing. Called once from main() right after
 * power_source_init(). */
int8_t LCD_run_pd_banner(void);

/* Boot-time progress display for the USB-PD probe (shown while
 * power_source_init() asks the source for each voltage/current combination).
 *
 *   LCD_show_pd_probe_start(): draws the header and one row per voltage
 *   LCD_show_pd_probe_result(): fills in the row with the discovered current
 *   LCD_show_pd_probe_trying(): live "trying ..." line at the bottom
 */
void LCD_show_pd_probe_start(const uint16_t *voltages_mv, uint8_t count);
void LCD_show_pd_probe_result(uint8_t row, uint16_t mv, uint16_t ma);
void LCD_show_pd_probe_trying(uint16_t mv, uint16_t ma);

/* Idle time after which the chooser closes on its own (ms). Any encoder
 * movement or button press restarts the countdown; only a button press changes
 * the selected mode. */
#define PD_BANNER_TIMEOUT_MS 20000

/* Convert a Celsius temperature to the user's preferred unit
 * (deg C or deg F per flash_values.deg_celsius). */
float convert_temperature(float temperature);

#endif /* DISPLAY_APP_H */
