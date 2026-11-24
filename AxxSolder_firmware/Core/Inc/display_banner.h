#ifndef DISPLAY_BANNER_H
#define DISPLAY_BANNER_H

#include <stdbool.h>

/* Opaque blue banners drawn over the station UI (240x320 panels):
 *
 *   - turning the knob shows the set temperature with big digits for 1 s;
 *   - clicking the knob opens a 2x2 preset picker, turning moves the
 *     selection, clicking again applies it and closes the banner.
 *
 * While a banner is up the caller pauses the normal screen redraws (the
 * graph history still keeps filling). display_banner_tick() is called from
 * display_app_tick() at the display rate; display_banner_encoder_click()
 * from the encoder button handler. */
void display_banner_tick(void);
void display_banner_encoder_click(void);

/* True while any banner covers the screen. */
bool display_banner_active(void);

/* True while the preset picker is open - the encoder is steering the
 * tiles, so encoder_read_set_temperature() must consume but ignore it. */
bool display_banner_preset_active(void);

/* Drop the banner state without repainting (caller is about to redraw). */
void display_banner_abort(void);

#endif /* DISPLAY_BANNER_H */
