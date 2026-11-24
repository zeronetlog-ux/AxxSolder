#include "display_banner.h"
#include "main.h"
#include "lcd.h"
#include "gui.h"
#include "app_colors.h"
#include "settings.h"
#include "encoder.h"
#include "display_app.h"
#include <stdio.h>

#if defined(LCD_240X320)

#define BANNER_TEMP_MS      1000u
#define BANNER_PRESET_MS   15000u

typedef struct {
    int16_t  bx, by, bw, bh;
    int16_t  val_x, val_y;
    int16_t  tile_x, tile_y, tile_w, tile_h, tile_gap;
} banner_layout_t;

static const banner_layout_t temp_portrait  = {20,105,200,110, 51,143,  0,  0,  0,  0, 0};
static const banner_layout_t temp_landscape = {50, 70,220,100, 91,103,  0,  0,  0,  0, 0};
static const banner_layout_t pres_portrait  = {10, 65,220,190,  0,  0, 22,103, 97, 68, 8};
static const banner_layout_t pres_landscape = {20, 30,280,180,  0,  0, 32, 70,124, 63, 8};

typedef enum { BANNER_NONE, BANNER_TEMP, BANNER_PRESET } banner_mode_t;

static banner_mode_t  banner_mode;
static uint32_t       banner_deadline;
static int            banner_last_set;
static uint8_t        banner_seen;
static uint8_t        preset_sel;
static banner_layout_t lay;

static bool portrait(void)
{
    return (flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2);
}

static void select_layout(bool preset)
{
    if (preset) {
        lay = portrait() ? pres_portrait : pres_landscape;
    } else {
        lay = portrait() ? temp_portrait : temp_landscape;
    }
}

static char unit_letter(void)
{
    return (flash_values.deg_celsius == 1) ? 'C' : 'F';
}

static uint16_t preset_value(uint8_t i)
{
    float v;

    switch (i) {
        case 0:  v = flash_values.preset_temp_1; break;
        case 1:  v = flash_values.preset_temp_2; break;
        case 2:  v = flash_values.preset_temp_3; break;
        default: v = flash_values.preset_temp_4; break;
    }
    return (uint16_t)((v < 0.0f) ? 0.0f : v);
}

static int set_value(void)
{
    return (int)convert_temperature(sensor_values.set_temperature);
}

static void draw_frame(void)
{
    UG_FillFrame((uint16_t)lay.bx, (uint16_t)lay.by,
                 (uint16_t)(lay.bx + lay.bw - 1), (uint16_t)(lay.by + lay.bh - 1),
                 C_BannerBlue);
    UG_DrawFrame((uint16_t)lay.bx, (uint16_t)lay.by,
                 (uint16_t)(lay.bx + lay.bw - 1), (uint16_t)(lay.by + lay.bh - 1),
                 C_BannerBlueEdge);
}

static void draw_unit(int x, int y)
{
    char u[2] = { unit_letter(), 0 };

    UG_DrawCircle(x + 2, y + 3, 2, C_Text);
    LCD_PutStr((uint16_t)(x + 6), (uint16_t)y, u, FONT_arial_12X15, C_Text, C_BannerBlue);
}

static void draw_temp_value(int v)
{
    char b[8];

    (void)snprintf(b, sizeof(b), "%d", v);
    LCD_PutStr((uint16_t)lay.val_x, (uint16_t)lay.val_y, b,
               FONT_arial_40X54_NUMBERS, C_Text, C_BannerBlue);
}

static void draw_temp_banner(void)
{
    select_layout(false);
    draw_frame();
    UG_FontSetTransparency(0);
    LCD_PutStr((uint16_t)(lay.bx + (lay.bw - 33) / 2), (uint16_t)(lay.by + 8),
               "SET", FONT_arial_17X18, C_Text, C_BannerBlue);
    draw_temp_value(set_value());
    draw_unit(lay.val_x + 125, lay.val_y + 3);
}

static int tile_x(uint8_t i)
{
    return lay.tile_x + (int)(i & 1u) * (lay.tile_w + lay.tile_gap);
}

static int tile_y(uint8_t i)
{
    return lay.tile_y + (int)(i >> 1) * (lay.tile_h + lay.tile_gap);
}

static void draw_tile(uint8_t i, bool selected)
{
    int      x  = tile_x(i);
    int      y  = tile_y(i);
    uint16_t bg = selected ? C_BannerBlueHi : C_BannerBlue;
    char     idx[2] = { (char)('1' + i), 0 };
    char     b[8];

    UG_FillFrame((uint16_t)x, (uint16_t)y,
                 (uint16_t)(x + lay.tile_w - 1), (uint16_t)(y + lay.tile_h - 1), bg);
    UG_DrawFrame((uint16_t)x, (uint16_t)y,
                 (uint16_t)(x + lay.tile_w - 1), (uint16_t)(y + lay.tile_h - 1),
                 selected ? C_Text : C_BannerBlueEdge);
    if (selected) {
        UG_DrawFrame((uint16_t)(x + 1), (uint16_t)(y + 1),
                     (uint16_t)(x + lay.tile_w - 2), (uint16_t)(y + lay.tile_h - 2),
                     C_Text);
    }
    LCD_PutStr((uint16_t)(x + 4), (uint16_t)(y + 3), idx, FONT_arial_12X15, C_Text, bg);
    (void)snprintf(b, sizeof(b), "%d", (int)convert_temperature((float)preset_value(i)));
    LCD_PutStr((uint16_t)(x + (lay.tile_w - 39) / 2),
               (uint16_t)(y + (lay.tile_h - 23) / 2 + 3),
               b, FONT_arial_20X23, C_Text, bg);
}

static void draw_preset_banner(void)
{
    int tx;

    select_layout(true);
    draw_frame();
    UG_FontSetTransparency(0);

    tx = lay.bx + (lay.bw - 80) / 2;
    LCD_PutStr((uint16_t)tx, (uint16_t)(lay.by + 8), "Preset",
               FONT_arial_20X23, C_Text, C_BannerBlue);
    draw_unit(tx + 64, lay.by + 12);

    for (uint8_t i = 0; i < 4; i++) {
        draw_tile(i, i == preset_sel);
    }
}

void display_banner_tick(void)
{
    uint32_t now = HAL_GetTick();
    int      cur;

    if (banner_mode == BANNER_PRESET) {
        int16_t d = encoder_read_detents();

        if (d != 0) {
            uint8_t old = preset_sel;

            preset_sel = (uint8_t)((preset_sel + d) & 3);
            if (preset_sel != old) {
                draw_tile(old, false);
                draw_tile(preset_sel, true);
            }
            banner_deadline = now + BANNER_PRESET_MS;
        }
        if ((int32_t)(now - banner_deadline) >= 0) {
            banner_mode = BANNER_NONE;
            display_app_draw_screen();
        }
        return;
    }

    cur = set_value();

    if (banner_mode == BANNER_TEMP) {
        if (cur != banner_last_set) {
            banner_last_set = cur;
            banner_deadline = now + BANNER_TEMP_MS;
            draw_temp_value(cur);
        }
        if ((int32_t)(now - banner_deadline) >= 0) {
            banner_mode = BANNER_NONE;
            display_app_draw_screen();
        }
        return;
    }

    if (!banner_seen) {
        banner_seen  = 1;
        banner_last_set = cur;
        return;
    }
    if (cur != banner_last_set) {
        banner_last_set = cur;
        banner_mode = BANNER_TEMP;
        banner_deadline = now + BANNER_TEMP_MS;
        draw_temp_banner();
    }
}

void display_banner_encoder_click(void)
{
    if (banner_mode == BANNER_PRESET) {
        encoder_set_count(preset_value(preset_sel));
        banner_last_set = set_value();
        banner_seen     = 1;
        banner_mode     = BANNER_NONE;
        display_app_draw_screen();
        return;
    }

    /* Open the picker on the tile closest to the current setpoint. */
    {
        int cur    = (int)sensor_values.set_temperature;
        int best   = 0;
        int best_d = 100000;

        for (uint8_t i = 0; i < 4; i++) {
            int d = cur - (int)preset_value(i);

            if (d < 0) {
                d = -d;
            }
            if (d < best_d) {
                best_d = d;
                best   = i;
            }
        }
        preset_sel = (uint8_t)best;
    }

    select_layout(true);
    (void)encoder_read_detents();          /* drop movement from before */
    banner_mode     = BANNER_PRESET;
    banner_deadline = HAL_GetTick() + BANNER_PRESET_MS;
    draw_preset_banner();
}

bool display_banner_active(void)
{
    return banner_mode != BANNER_NONE;
}

bool display_banner_preset_active(void)
{
    return banner_mode == BANNER_PRESET;
}

void display_banner_abort(void)
{
    banner_mode = BANNER_NONE;
}

#else

void display_banner_tick(void) {}
void display_banner_encoder_click(void) {}
bool display_banner_active(void) { return false; }
bool display_banner_preset_active(void) { return false; }
void display_banner_abort(void) {}

#endif /* LCD_240X320 */
