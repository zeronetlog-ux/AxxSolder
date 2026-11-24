#include "display_app.h"
#include "display_banner.h"
#include "app_colors.h"   /* AxxSolder UI colour palette */
#include "main.h"
#include "util.h"
#include "settings.h"
#include "handle.h"
#include "heater.h"
#include "power_source.h"
#include "buttons.h"
#include "encoder.h"
#include "lcd.h"
#include "gui.h"
#include "graph.h"
#include "moving_average.h"
#include "stusb4500.h"    /* NVM dump screen */
#include <stdio.h>
#include <string.h>
#include <math.h>

#define POPUP_DWELL_MS      2000

static uint32_t previous_millis_display = 0;
static uint32_t previous_millis_popup   = 0;

static char DISPLAY_buffer[40];

/* Set when the SLEEP/STANDBY label has been drawn into the bar so we
 * don't redraw the same overlay every tick. File-scope so callers
 * (buttons.c after preset save) can clear them directly. */
uint8_t sleep_state_written_to_LCD   = 0;
uint8_t standby_state_written_to_LCD = 0;

static uint8_t popup_shown = 0;

/* Filter for the smoothed power-bar reading. main.c initializes it. */
FilterTypeDef requested_power_filtered_filter_struct;

/* Return the temperature in the correct unit */
float convert_temperature(float temperature){
	if (flash_values.deg_celsius == 1){
		return temperature;
	}
	else{
		return ((temperature * 9) + 3) / 5 + 32;
	}
}

/* Handle name for the info line ("---" when no known handle is detected). */
static const char *handle_name_str(void)
{
    switch (attached_handle) {
    case T210:  return "T210";
    case T245:  return "T245";
    case NT115: return "NT115";
    default:    return "---";
    }
}

/* Power-source label for the info line. For USB-PD the negotiated contract
 * power (sensor_values.USB_PD_power_limit, watts) is appended. */
static void format_power_source(char *buf, size_t len)
{
    switch (power_source) {
    case POWER_USB:
        if (pd_status == PD_STATUS_NO_CAPS) {
            /* Only the USB default (5 V) contract is in force: the source's
             * capability list could not be read (see stusb4500.h). */
            (void)snprintf(buf, len, "PD?R");
        } else if ((sensor_values.USB_PD_contract_power > 0.0f) &&
                   (sensor_values.USB_PD_contract_power < 1000.0f)) {
            /* What the source granted - the heater budget is slightly lower,
             * the difference stays inside the firmware. */
            (void)snprintf(buf, len, "PD %dW", (int)(sensor_values.USB_PD_contract_power + 0.5f));
        } else {
            (void)snprintf(buf, len, "PD");
        }
        break;

    case POWER_DC:
        /* A PD cable that delivered power but whose contract we could not
         * detect is the interesting case here, so the reason is shown:
         *   XT        - no PD cable attached (plain barrel-jack supply)
         *   XT?id     - STUSB4500 not found / I2C failure (id = 0 on failure)
         *   PD?Cid    - cable attached, but no PD contract was negotiated     */
        if (pd_status == PD_STATUS_NO_CONTRACT) {
            (void)snprintf(buf, len, "PD?C%02X", (unsigned)pd_chip_id);
        } else if (pd_status == PD_STATUS_NO_CHIP) {
            (void)snprintf(buf, len, "XT?%02X", (unsigned)pd_chip_id);
        } else {
            (void)snprintf(buf, len, "XT");
        }
        break;

    case POWER_BAT:
        (void)snprintf(buf, len, "BAT");
        break;

    default:
        (void)snprintf(buf, len, "---");
        break;
    }
}

/* Draw "Handle:<name>" and "Vin:<volts> <source>" (source after the voltage,
 * with the negotiated power for USB-PD). Used by both screen orientations. */
static void draw_info_lines(uint16_t x, uint16_t y_handle, uint16_t y_vin)
{
    char src[12];

    (void)snprintf(DISPLAY_buffer, sizeof(DISPLAY_buffer), "Handle:%-5s", handle_name_str());
    LCD_PutStr(x, y_handle, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);

    format_power_source(src, sizeof(src));
    (void)snprintf(DISPLAY_buffer, sizeof(DISPLAY_buffer), "Vin:%d.%d %-7s",
                   (int)sensor_values.bus_voltage,
                   (int)(sensor_values.bus_voltage * 10) % 10,
                   src);
    LCD_PutStr(x, y_vin, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);
}

/* Formatting float into a string as right-aligned, by adding spaces on the left */
static void format_number_right(float input, char* buffer) {
    int rounded = (int)clamp(roundf(input), 0, 999);

    char number_str[4]; // Enough for "100" + '\0'
    sprintf(number_str, "%d", rounded);

    int num_digits = strlen(number_str);
    int padding_spaces = (3 - num_digits) * 2;

    // Fill padding spaces first
    memset(buffer, ' ', padding_spaces);
    // Copy the number after the padding
    strcpy(buffer + padding_spaces, number_str);

    // Null terminate the full string
    buffer[padding_spaces + num_digits] = '\0';
}

/* Formatting float into a string as left-aligned, by adding spaces on the right */
static void format_number_left(float input, char* buffer) {
    int rounded = (int)clamp(roundf(input), 0, 999);

    char number_str[4]; // "100" + '\0'
    sprintf(number_str, "%d", rounded);

    int num_digits = strlen(number_str);
    int padding_spaces = (3 - num_digits) * 2;

    // Copy number into buffer
    strcpy(buffer, number_str);

    // Add spaces to the right
    memset(buffer + num_digits, ' ', padding_spaces);
    buffer[num_digits + padding_spaces] = '\0';
}

/* Function to update the standby and sleep timer countdown */
/* ------------------------------------------------------------------ */
/* Graph view update                                                  */
/* ------------------------------------------------------------------ */

#if defined(LCD_240X320)

typedef struct {
    int16_t tip_temp;
    int16_t set_temp;
    uint8_t celsius;
    uint8_t detached;
    uint8_t temp_mode;
    char    info[4][20];
} graph_readout_cache_t;

static graph_readout_cache_t graph_readout;
static uint8_t graph_readout_valid = 0;

void display_app_graph_invalidate(void)
{
    graph_readout_valid = 0;
}

/* Supply power budget: the USB-PD contract when present, otherwise the
 * configured DC budget. Used by the Pwr readout and the gauge scale. */
static int supply_power_w(void)
{
    int power_w = (int)(sensor_values.USB_PD_contract_power + 0.5f);

    if ((power_w <= 0) || (power_w >= 1000)) {
        power_w = (int)(sensor_values.max_power_watt + 0.5f);
    }
    return power_w;
}

/* Cartridge / supply / MCU pane (replaces JBC's Time/Energy table). */
static void build_graph_info(char info[4][20])
{
    memset(info, 0, 4 * 20);
    (void)snprintf(info[0], sizeof(info[0]), "%s", handle_name_str());
    (void)snprintf(info[1], sizeof(info[1]), "%d.%dV",
                   (int)sensor_values.bus_voltage,
                   (int)(sensor_values.bus_voltage * 10) % 10);

    /* What the source granted over USB-PD (or the supply budget on DC). */
    (void)snprintf(info[2], sizeof(info[2]), "%dW", supply_power_w());
}

static void update_graph_display(void)
{
    const graph_layout_t *L = &graph_layout;
    const char unit = (flash_values.deg_celsius == 1) ? 'C' : 'F';
    char info[4][20];
    uint8_t detached = (uint8_t)(cartridge_state == DETACHED);
    uint8_t celsius  = (uint8_t)(flash_values.deg_celsius == 1);
    /* 0 = RUN (white), 1 = standby (yellow), 2 = sleep/off (blue). */
    uint8_t temp_mode;
    uint16_t temp_color;

    if (sensor_values.current_state == RUN) {
        temp_mode = 0;
        temp_color = C_Text;
    } else if ((sensor_values.current_state == STANDBY) ||
               (sensor_values.current_state == PRESTANDBY)) {
        temp_mode = 1;
        temp_color = C_Header;
    } else {
        temp_mode = 2;
        temp_color = C_TempActive;
    }
    int set_temp = (int)convert_temperature(sensor_values.set_temperature);
    int tip_temp = detached ? -1000
                            : (int)convert_temperature(sensor_values.thermocouple_temperature_filtered);
    char ub[2] = { unit, 0 };

    build_graph_info(info);

    /* Large actual tip temperature, centred in the left pane. Only the
     * 3-character field is repainted on a value change, otherwise the big
     * digits visibly flash. */
    {
        const int field_w = 99;               /* 3 chars in the numeric face */
        const int group_w = field_w + 18;     /* field + degree ring + 'C'   */
        int x = (L->div_x - group_w) / 2;
        bool state_changed = !graph_readout_valid ||
                             (graph_readout.detached != detached) ||
                             (graph_readout.celsius != celsius) ||
                             (graph_readout.temp_mode != temp_mode);

        if (x < L->big_x) {
            x = L->big_x;
        }

        if (state_changed) {
            UG_FillFrame(L->big_x, L->big_y,
                         (uint16_t)(L->div_x - 6), (uint16_t)(L->sel_y - 6), C_Background);

            if (detached) {
                int dx = (L->div_x - 60) / 2;

                if (dx < L->big_x) {
                    dx = L->big_x;
                }
                LCD_PutStr((uint16_t)dx, L->big_y, "---",
                           FONT_arial_36X44_NUMBERS, temp_color, C_Background);
            } else {
                char text[8];

                format_number_left(convert_temperature(sensor_values.thermocouple_temperature_filtered), text);
                LCD_PutStr((uint16_t)x, L->big_y, text,
                           FONT_arial_36X44_NUMBERS, temp_color, C_Background);
                UG_DrawCircle(x + field_w + 5, L->big_y + 6, 2, temp_color);
                UG_DrawCircle(x + field_w + 5, L->big_y + 6, 1, temp_color);
                LCD_PutStr((uint16_t)(x + field_w + 9), (uint16_t)(L->big_y + 2), ub,
                           FONT_arial_12X15, temp_color, C_Background);
            }
            graph_readout.tip_temp = (int16_t)tip_temp;
            graph_readout.detached = detached;
            graph_readout.celsius  = celsius;
            graph_readout.temp_mode = temp_mode;
        } else if (!detached && (graph_readout.tip_temp != tip_temp)) {
            char text[8];

            format_number_left(convert_temperature(sensor_values.thermocouple_temperature_filtered), text);
            LCD_PutStr((uint16_t)x, L->big_y, text,
                       FONT_arial_36X44_NUMBERS, temp_color, C_Background);
            graph_readout.tip_temp = (int16_t)tip_temp;
        }
    }

    /* Selected temperature, centred under the big readout. */
    if (!graph_readout_valid || (graph_readout.set_temp != set_temp)) {
        char text[24];
        const int group_w = 85;          /* "Sel. %3d" in 20x23 */
        int x = (L->div_x - group_w) / 2;

        if (x < L->big_x) {
            x = L->big_x;
        }
        UG_FillFrame((uint16_t)x, (uint16_t)(L->sel_y - 2),
                     (uint16_t)(x + group_w), (uint16_t)(L->sel_y + 23), C_Background);
        (void)snprintf(text, sizeof(text), "Sel. %3d", set_temp);
        LCD_PutStr((uint16_t)x, L->sel_y, text, FONT_arial_20X23, C_JbcGrayLight, C_Background);

        graph_readout.set_temp = (int16_t)set_temp;
    }

    /* Info pane values. */
    if (!graph_readout_valid || (memcmp(graph_readout.info, info, sizeof(info)) != 0)) {
        for (uint8_t i = 0; i < 3; i++) {
            uint16_t y = (uint16_t)(L->info_y0 + (i * L->info_pitch));

            if (!graph_readout_valid ||
                (memcmp(graph_readout.info[i], info[i], sizeof(info[i])) != 0)) {
                UG_FillFrame(L->info_value_x, y,
                             (uint16_t)((((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2)) ? 240 : 320) - 6),
                             (uint16_t)(y + 23), C_Background);
                LCD_PutStr(L->info_value_x, y, info[i],
                           FONT_arial_20X23, C_Text, C_Background);
            }
        }
        memcpy(graph_readout.info, info, sizeof(info));
    }

    graph_readout_valid = 1;
}

/* ------------------------------------------------------------------ */
/* Alternative view: big tip temperature, power gauge, info pane      */
/* ------------------------------------------------------------------ */

static uint8_t alt_view = 0;

typedef struct {
    uint16_t big_x, big_y, sel_y;
    uint16_t div_x, div_y;
    uint16_t info_x, info_value_x, info_y0, info_pitch;
    uint16_t cx, cy, r;
    uint16_t title_x, title_y, value_y;
    uint16_t s25_x, s25_y, s75_x, s75_y;
} alt_layout_t;

static const alt_layout_t alt_portrait = {
    .big_x = 51, .big_y = 53, .sel_y = 117,
    .div_x = 120, .div_y = 160,
    .info_x = 122, .info_value_x = 172, .info_y0 = 180, .info_pitch = 30,
    .cx = 60, .cy = 292, .r = 46,
    .title_x = 4, .title_y = 165, .value_y = 195,
    .s25_x = 2, .s25_y = 294, .s75_x = 78, .s75_y = 294,
};

static const alt_layout_t alt_landscape = {
    .big_x = 91, .big_y = 33, .sel_y = 93,
    .div_x = 160, .div_y = 120,
    .info_x = 166, .info_value_x = 214, .info_y0 = 140, .info_pitch = 30,
    .cx = 80, .cy = 225, .r = 48,
    .title_x = 4, .title_y = 125, .value_y = 148,
    .s25_x = 2, .s25_y = 190, .s75_x = 118, .s75_y = 190,
};

static alt_layout_t alt;
static int gauge_max_w = 0;
static int gauge_needle_w = -1;

static void alt_draw_gauge_static(int max_w);
static void alt_draw_needle(int watt);

static void alt_select_layout(void)
{
    alt = ((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2))
        ? alt_portrait : alt_landscape;
}

static uint16_t alt_screen_width(void)
{
    return ((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2)) ? 240 : 320;
}

static uint16_t alt_screen_height(void)
{
    return ((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2)) ? 320 : 240;
}

static void alt_draw_static(void)
{
    const uint16_t sw = alt_screen_width();
    const uint16_t sh = alt_screen_height();
    static const char *names[3] = {"Tip", "Vin", "Pwr"};

    alt_select_layout();
    UG_FillScreen(C_Background);
    UG_FontSetTransparency(0);

    UG_DrawLine(0, alt.div_y, sw - 1, alt.div_y, C_JbcGrid);
    UG_DrawLine(alt.div_x, alt.div_y + 1, alt.div_x, sh - 1, C_JbcGrid);

    for (uint8_t i = 0; i < 3; i++) {
        uint16_t y = (uint16_t)(alt.info_y0 + (i * alt.info_pitch));

        LCD_PutStr(alt.info_x, y, (char *)names[i], FONT_arial_20X23, C_Text, C_Background);
        if (i) {
            draw_dashed_line_fillframe(alt.info_x, (int16_t)(y - 5), (int16_t)(sw - 6),
                                       (int16_t)(y - 5), C_JbcGrid, 2, 2, 1);
        }
    }

    display_app_graph_invalidate();
    gauge_max_w = 0;
    gauge_needle_w = -1;
}

static void alt_update_display(void)
{
    const uint16_t sw = alt_screen_width();
    const char unit = (flash_values.deg_celsius == 1) ? 'C' : 'F';
    char info[4][20];
    char ub[2] = { unit, 0 };
    uint8_t detached = (uint8_t)(cartridge_state == DETACHED);
    uint8_t celsius  = (uint8_t)(flash_values.deg_celsius == 1);
    uint8_t temp_mode;
    uint16_t temp_color;
    int set_temp = (int)convert_temperature(sensor_values.set_temperature);
    int tip_temp = detached ? -1000
                            : (int)convert_temperature(sensor_values.thermocouple_temperature_filtered);

    if (sensor_values.current_state == RUN) {
        temp_mode = 0;
        temp_color = C_Text;
    } else if ((sensor_values.current_state == STANDBY) ||
               (sensor_values.current_state == PRESTANDBY)) {
        temp_mode = 1;
        temp_color = C_Header;
    } else {
        temp_mode = 2;
        temp_color = C_TempActive;
    }

    alt_select_layout();
    build_graph_info(info);

    /* Large actual tip temperature, centred in the upper half. */
    {
        const int field_w = 120;
        const int group_w = field_w + 18;
        const int x = alt.big_x;
        bool state_changed = !graph_readout_valid ||
                             (graph_readout.detached != detached) ||
                             (graph_readout.celsius != celsius) ||
                             (graph_readout.temp_mode != temp_mode);

        if (state_changed) {
            UG_FillFrame((uint16_t)(x - 2), (uint16_t)(alt.big_y - 2),
                         (uint16_t)(x + group_w), (uint16_t)(alt.big_y + 58), C_Background);
            if (detached) {
                LCD_PutStr((uint16_t)(x + 19), alt.big_y, "---",
                           FONT_arial_40X54_NUMBERS, temp_color, C_Background);
            } else {
                char text[8];

                format_number_left(convert_temperature(sensor_values.thermocouple_temperature_filtered), text);
                LCD_PutStr((uint16_t)x, alt.big_y, text,
                           FONT_arial_40X54_NUMBERS, temp_color, C_Background);
                UG_DrawCircle(x + field_w + 5, alt.big_y + 8, 2, temp_color);
                UG_DrawCircle(x + field_w + 5, alt.big_y + 8, 1, temp_color);
                LCD_PutStr((uint16_t)(x + field_w + 9), (uint16_t)(alt.big_y + 3), ub,
                           FONT_arial_12X15, temp_color, C_Background);
            }
            graph_readout.tip_temp = (int16_t)tip_temp;
            graph_readout.detached = detached;
            graph_readout.celsius  = celsius;
            graph_readout.temp_mode = temp_mode;
        } else if (!detached && (graph_readout.tip_temp != tip_temp)) {
            char text[8];

            format_number_left(convert_temperature(sensor_values.thermocouple_temperature_filtered), text);
            LCD_PutStr((uint16_t)x, alt.big_y, text,
                       FONT_arial_40X54_NUMBERS, temp_color, C_Background);
            graph_readout.tip_temp = (int16_t)tip_temp;
        }
    }

    /* Selected temperature, right-aligned under the big number. */
    if (!graph_readout_valid || (graph_readout.set_temp != set_temp)) {
        const int group_w = 85;              /* "Sel. %3d" in 20x23 */
        const int x = sw - 6 - group_w;
        char text[32];

        UG_FillFrame((uint16_t)x, (uint16_t)(alt.sel_y - 2),
                     (uint16_t)(x + group_w), (uint16_t)(alt.sel_y + 23), C_Background);
        (void)snprintf(text, sizeof(text), "Sel. %3d", set_temp);
        LCD_PutStr((uint16_t)x, alt.sel_y, text, FONT_arial_20X23, C_JbcGrayLight, C_Background);
        graph_readout.set_temp = (int16_t)set_temp;
    }

    /* Info pane values (Tip / Vin / Pwr). */
    if (!graph_readout_valid || (memcmp(graph_readout.info, info, sizeof(info)) != 0)) {
        for (uint8_t i = 0; i < 3; i++) {
            uint16_t y = (uint16_t)(alt.info_y0 + (i * alt.info_pitch));

            if (!graph_readout_valid ||
                (memcmp(graph_readout.info[i], info[i], sizeof(info[i])) != 0)) {
                UG_FillFrame(alt.info_value_x, y, (uint16_t)(sw - 6),
                             (uint16_t)(y + 23), C_Background);
                LCD_PutStr(alt.info_value_x, y, info[i], FONT_arial_20X23, C_Text, C_Background);
            }
        }
        memcpy(graph_readout.info, info, sizeof(info));
    }

    /* Power gauge: scale in watts from the supply budget. */
    {
        int max_w = supply_power_w();
        int current_w;

        if (max_w < 1) {
            max_w = 1;
        }
        if (max_w > 999) {
            max_w = 999;
        }
        current_w = (int)(sensor_values.requested_power_filtered * (float)max_w / PID_MAX_OUTPUT + 0.5f);
        if (current_w < 0) {
            current_w = 0;
        }
        if (current_w > max_w) {
            current_w = max_w;
        }

        if (!graph_readout_valid || (max_w != gauge_max_w)) {
            alt_draw_gauge_static(max_w);
        }
        if (current_w != gauge_needle_w) {
            alt_draw_needle(current_w);
        }
    }

    graph_readout_valid = 1;
}

/* sin(deg) * 255 for 0..90 degrees (one byte per entry). */
static const uint8_t gauge_sin_tab[91] = {
    0, 4, 9, 13, 18, 22, 27, 31, 35, 40, 44, 49, 53, 57, 62, 66, 70, 75,
    79, 83, 87, 91, 96, 100, 104, 108, 112, 116, 120, 124, 127, 131, 135,
    139, 143, 146, 150, 153, 157, 160, 164, 167, 171, 174, 177, 180, 183,
    186, 190, 192, 195, 198, 201, 204, 206, 209, 211, 214, 216, 219, 221,
    223, 225, 227, 229, 231, 233, 235, 236, 238, 240, 241, 243, 244, 245,
    246, 247, 248, 249, 250, 251, 252, 253, 253, 254, 254, 254, 255, 255,
    255, 255,
};

static int16_t gauge_sin(int deg)
{
    return (deg <= 90) ? (int16_t)gauge_sin_tab[deg] : (int16_t)gauge_sin_tab[180 - deg];
}

static int16_t gauge_cos(int deg)
{
    return (deg <= 90) ? (int16_t)gauge_sin_tab[90 - deg] : -(int16_t)gauge_sin_tab[deg - 90];
}

static void alt_needle_endpoint(int deg, int *x2, int *y2)
{
    *x2 = (int)alt.cx + (((int)alt.r - 14) * gauge_cos(deg)) / 255;
    *y2 = (int)alt.cy - (((int)alt.r - 14) * gauge_sin(deg)) / 255;
}

/* Static gauge: title, 25/50/75 % scale values in watts, arc and ticks. */
static void alt_draw_gauge_static(int max_w)
{
    const uint16_t sh = alt_screen_height();
    char b[8];
    int v;
    int w;
    int x;

    gauge_max_w = max_w;
    gauge_needle_w = -1;

    /* Clear the left half of the bottom pane. */
    UG_FillFrame(0, (uint16_t)(alt.div_y + 1), (uint16_t)(alt.div_x - 1),
                 (uint16_t)(sh - 1), C_Background);

    LCD_PutStr(alt.title_x, alt.title_y, "Power", FONT_arial_17X18, C_Text, C_Background);

    /* Mid-scale value (the three numbers are the scale, not the reading). */
    v = (max_w * 50 + 50) / 100;
    (void)snprintf(b, sizeof(b), "%dW", v);
    w = (v >= 100) ? 60 : ((v >= 10) ? 45 : 28);
    x = (int)alt.cx - (w / 2);
    LCD_PutStr((uint16_t)x, alt.value_y, b, FONT_arial_20X23, C_Text, C_Background);

    v = (max_w * 25 + 50) / 100;
    (void)snprintf(b, sizeof(b), "%dW", v);
    UG_FillFrame((uint16_t)(alt.s25_x - 1), (uint16_t)(alt.s25_y - 1),
                 (uint16_t)(alt.s25_x + 41), (uint16_t)(alt.s25_y + 18), C_Background);
    LCD_PutStr(alt.s25_x, alt.s25_y, b, FONT_arial_17X18, C_Text, C_Background);

    v = (max_w * 75 + 50) / 100;
    (void)snprintf(b, sizeof(b), "%dW", v);
    UG_FillFrame((uint16_t)(alt.s75_x - 1), (uint16_t)(alt.s75_y - 1),
                 (uint16_t)(alt.s75_x + 41), (uint16_t)(alt.s75_y + 18), C_Background);
    LCD_PutStr(alt.s75_x, alt.s75_y, b, FONT_arial_17X18, C_Text, C_Background);

    for (int deg = 0; deg <= 180; deg++) {
        int px = (int)alt.cx + ((int)alt.r * gauge_cos(deg)) / 255;
        int py = (int)alt.cy - ((int)alt.r * gauge_sin(deg)) / 255;

        UG_DrawPixel((int16_t)px, (int16_t)py, C_Text);
    }

    for (int i = 0; i <= 10; i++) {
        int deg = i * 18;
        int c = gauge_cos(deg);
        int s = gauge_sin(deg);
        int x1 = (int)alt.cx + (((int)alt.r - 8) * c) / 255;
        int y1 = (int)alt.cy - (((int)alt.r - 8) * s) / 255;
        int x2 = (int)alt.cx + ((int)alt.r * c) / 255;
        int y2 = (int)alt.cy - ((int)alt.r * s) / 255;

        draw_line_with_thickness(x1, y1, x2, y2, C_Text, 1);
    }
}

/* Move the needle without repainting the gauge: erase the old hand, put the
 * arc/ticks it covered back, then draw the new hand. */
static void alt_draw_needle(int watt)
{
    const uint16_t sh = alt_screen_height();
    int new_deg;
    int nx;
    int ny;

    if (gauge_max_w < 1) {
        return;
    }
    if (watt < 0) {
        watt = 0;
    }
    if (watt > gauge_max_w) {
        watt = gauge_max_w;
    }
    new_deg = 180 - (watt * 180) / gauge_max_w;
    alt_needle_endpoint(new_deg, &nx, &ny);

    if (gauge_needle_w >= 0) {
        int ow = gauge_needle_w;
        int ox;
        int oy;
        int b0x;
        int b0y;
        int b1x;
        int b1y;

        if (ow > gauge_max_w) {
            ow = gauge_max_w;
        }
        alt_needle_endpoint(180 - (ow * 180) / gauge_max_w, &ox, &oy);

        b0x = b1x = (int)alt.cx;
        b0y = b1y = (int)alt.cy;
        if (ox < b0x) b0x = ox; else if (ox > b1x) b1x = ox;
        if (oy < b0y) b0y = oy; else if (oy > b1y) b1y = oy;
        if (nx < b0x) b0x = nx; else if (nx > b1x) b1x = nx;
        if (ny < b0y) b0y = ny; else if (ny > b1y) b1y = ny;
        b0x -= 3; b0y -= 3; b1x += 3; b1y += 3;
        if (b0x < 0) b0x = 0;
        if (b0y < (int)alt.div_y + 1) b0y = (int)alt.div_y + 1;
        if (b1x > (int)alt.div_x - 1) b1x = (int)alt.div_x - 1;
        if (b1y > (int)sh - 1) b1y = (int)sh - 1;
        if (b1y > (int)alt.cy + 1) b1y = (int)alt.cy + 1;

        UG_FillFrame((uint16_t)b0x, (uint16_t)b0y, (uint16_t)b1x, (uint16_t)b1y, C_Background);

        for (int deg = 0; deg <= 180; deg++) {
            int ax = (int)alt.cx + ((int)alt.r * gauge_cos(deg)) / 255;
            int ay = (int)alt.cy - ((int)alt.r * gauge_sin(deg)) / 255;

            if ((ax >= b0x) && (ax <= b1x) && (ay >= b0y) && (ay <= b1y)) {
                UG_DrawPixel((int16_t)ax, (int16_t)ay, C_Text);
            }
        }

        for (int i = 0; i <= 10; i++) {
            int deg = i * 18;
            int c = gauge_cos(deg);
            int s = gauge_sin(deg);
            int x1 = (int)alt.cx + (((int)alt.r - 8) * c) / 255;
            int y1 = (int)alt.cy - (((int)alt.r - 8) * s) / 255;
            int x2 = (int)alt.cx + ((int)alt.r * c) / 255;
            int y2 = (int)alt.cy - ((int)alt.r * s) / 255;
            int lx = (x1 < x2) ? x1 : x2;
            int hx = (x1 < x2) ? x2 : x1;
            int ly = (y1 < y2) ? y1 : y2;
            int hy = (y1 < y2) ? y2 : y1;

            if ((hx >= b0x) && (lx <= b1x) && (hy >= b0y) && (ly <= b1y)) {
                draw_line_with_thickness(x1, y1, x2, y2, C_Text, 1);
            }
        }
    }

    draw_line_with_thickness((int)alt.cx, (int)alt.cy, nx, ny, C_Text, 3);
    UG_DrawCircle(alt.cx, alt.cy, 5, C_TempActive);
    UG_DrawCircle(alt.cx, alt.cy, 4, C_TempActive);
    UG_DrawCircle(alt.cx, alt.cy, 3, C_TempActive);
    gauge_needle_w = watt;
}

void display_app_toggle_view(void)
{
    alt_view ^= 1u;
    display_app_graph_invalidate();
    display_app_draw_screen();
}

#else
/* Other panel geometries keep the legacy text screen; nothing to cache. */
void display_app_graph_invalidate(void)
{
}

void display_app_toggle_view(void)
{
}
#endif /* LCD_240X320 */


#if !defined(LCD_240X320)
/* Legacy text main screen. The 240x320 stations always use the JBC graph /
 * gauge screens, so this code is compiled out there to save flash. */
static void update_standby_sleep_display(void) {
    uint32_t now = HAL_GetTick();
    uint32_t countdown_ms = 0;
    char label[16] = {0};
    char display_buffer[40]  = {0};
    // clean field
    if(sensor_values.current_state == SLEEP || sensor_values.current_state == RUN || sensor_values.current_state == HALTED || sensor_values.current_state == EMERGENCY_SLEEP){
    	LCD_PutStr(11, 235, "                            ", FONT_arial_17X18, C_Header, C_Background);
    }

    switch(sensor_values.current_state) {
        case PRESTANDBY:
            // Countdown to STANDBY
            countdown_ms = flash_values.standby_delay * 1000UL - (now - previous_millis_prestandby);
            if(countdown_ms > flash_values.standby_delay * 1000UL) countdown_ms = 0; // sanity
            strcpy(label, "To Standby");
            break;

        case STANDBY:
            // Countdown to SLEEP
            countdown_ms = flash_values.standby_time * 60000UL - (now - previous_millis_standby);
            if(countdown_ms > flash_values.standby_time * 60000UL) countdown_ms = 0; // sanity
            strcpy(label, "To Sleep");
            break;

        default:
            countdown_ms = 0;
            label[0] = '\0'; // no label
            break;
    }

    if(countdown_ms > 0 && label[0] != '\0') {
        uint32_t min = countdown_ms / 60000UL;
        uint32_t sec = (countdown_ms % 60000UL) / 1000UL;
        sprintf(display_buffer, "%s: %02lu:%02lu     ", label, min, sec);
    } else {
        strcpy(display_buffer, "                    "); // clear display or show nothing
    }
	LCD_PutStr(11, 235, display_buffer, FONT_arial_17X18, C_Header, C_Background);
}

static void update_display(void) {
    if (flash_values.screen_rotation == 0) {
        update_standby_sleep_display();
    }
    float filtered_power_percent = sensor_values.requested_power_filtered / PID_MAX_OUTPUT;
    float filtered_power_watt    = sensor_values.max_power_watt * filtered_power_percent;
    uint16_t bar_top = 0;

    if ((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2)) {
        memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
        format_number_left(convert_temperature(sensor_values.set_temperature), DISPLAY_buffer);
        LCD_PutStr(19, 33, DISPLAY_buffer, FONT_arial_36X44_NUMBERS, C_Text, C_Background);

        if (cartridge_state == DETACHED) {
            LCD_PutStr(15, 117, " ---  ", FONT_arial_36X44_NUMBERS, C_Text, C_Background);
        } else {
            memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
            format_number_left(convert_temperature(sensor_values.thermocouple_temperature_filtered), DISPLAY_buffer);
            LCD_PutStr(19, 117, DISPLAY_buffer, FONT_arial_36X44_NUMBERS, C_Text, C_Background);
        }

        /* Handle + input voltage + power source (PD shows negotiated power). */
        draw_info_lines(11, 192, 212);

        memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
        if (convert_temperature(sensor_values.mcu_temperature) < 99.5f) {
            sprintf(DISPLAY_buffer, "%d", (int)convert_temperature(sensor_values.mcu_temperature));
        } else {
            sprintf(DISPLAY_buffer, "%d", (int)convert_temperature(sensor_values.mcu_temperature));
        }
        LCD_PutStr(59, 275, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);

        if (sensor_values.max_power_watt < 100) {
            sprintf(DISPLAY_buffer, "  %d W", (int)sensor_values.max_power_watt);
        } else {
            sprintf(DISPLAY_buffer, "%d W", (int)sensor_values.max_power_watt);
        }
        LCD_PutStr(183, 8, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);

        memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
        if (flash_values.power_unit == 0) {
            format_number_right(filtered_power_watt, DISPLAY_buffer);
        } else {
            format_number_right(100 * filtered_power_percent, DISPLAY_buffer);
        }
        LCD_PutStr(186, 275, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);

        if ((sensor_values.current_state == SLEEP || sensor_values.current_state == EMERGENCY_SLEEP || sensor_values.current_state == HALTED) && !sleep_state_written_to_LCD) {
            UG_FillFrame(210, 66, 230, 268, C_Highlight);
            LCD_PutStr(214, 73,  "Z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(216, 99,  "z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(214, 129, "Z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(216, 158, "z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(214, 191, "Z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(216, 217, "z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(214, 247, "Z", FONT_arial_20X23, C_Background, C_Highlight);
            sleep_state_written_to_LCD = 1;
            standby_state_written_to_LCD = 0;
        }
        else if ((sensor_values.current_state == STANDBY) && !standby_state_written_to_LCD) {
            UG_FillFrame(210, 66, 230, 268, C_Highlight);
            LCD_PutStr(213, 73,  "S", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(213, 102, "T", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(213, 131, "A", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(213, 160, "N", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(213, 189, "D", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(213, 218, "B", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(213, 247, "Y", FONT_arial_20X23, C_Background, C_Highlight);
            standby_state_written_to_LCD = 1;
            sleep_state_written_to_LCD = 0;
        }
        else if (sensor_values.current_state == RUN) {
            bar_top = 268 - (uint16_t)(filtered_power_percent * 202.0f);
            UG_FillFrame(210, bar_top, 230, 268,     C_PowerBar);
            UG_FillFrame(210, 66,      230, bar_top, C_Background);
            standby_state_written_to_LCD = 0;
            sleep_state_written_to_LCD = 0;
        }
    }
    else {
        memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
        format_number_left(convert_temperature(sensor_values.set_temperature), DISPLAY_buffer);
        LCD_PutStr(64, 35, DISPLAY_buffer, FONT_arial_36X44_NUMBERS, C_Text, C_Background);

        if (cartridge_state == DETACHED) {
            LCD_PutStr(60, 115, " ---  ", FONT_arial_36X44_NUMBERS, C_Text, C_Background);
        } else {
            memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
            format_number_left(convert_temperature(sensor_values.thermocouple_temperature_filtered), DISPLAY_buffer);
            LCD_PutStr(64, 115, DISPLAY_buffer, FONT_arial_36X44_NUMBERS, C_Text, C_Background);
        }

        /* Handle + input voltage + power source (PD shows negotiated power). */
        draw_info_lines(56, 175, 195);

        memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
        if (convert_temperature(sensor_values.mcu_temperature) < 99.5f) {
            sprintf(DISPLAY_buffer, "  %d", (int)convert_temperature(sensor_values.mcu_temperature));
        } else {
            sprintf(DISPLAY_buffer, "%d", (int)convert_temperature(sensor_values.mcu_temperature));
        }
        LCD_PutStr(102, 215, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);

        if (sensor_values.max_power_watt < 100) {
            sprintf(DISPLAY_buffer, "%d W  ", (int)sensor_values.max_power_watt);
        } else {
            sprintf(DISPLAY_buffer, "%d W", (int)sensor_values.max_power_watt);
        }
        LCD_PutStr(2, 10, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);

        memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
        if (flash_values.power_unit == 0) { // power unit is set to W
            format_number_right(filtered_power_watt, DISPLAY_buffer);
        } else {
            format_number_right(100 * filtered_power_percent, DISPLAY_buffer);
        }
        LCD_PutStr(5, 215, DISPLAY_buffer, FONT_arial_17X18, C_Text, C_Background);

        if ((sensor_values.current_state == SLEEP || sensor_values.current_state == EMERGENCY_SLEEP || sensor_values.current_state == HALTED) && !sleep_state_written_to_LCD) {
            UG_FillFrame(10, 32, 30, 209, C_Highlight);
            LCD_PutStr(14, 41,  "Z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(16, 68,  "z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 95,  "Z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(16, 122, "z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 149, "Z", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(16, 176, "z", FONT_arial_20X23, C_Background, C_Highlight);
            sleep_state_written_to_LCD = 1;
            standby_state_written_to_LCD = 0;
        }
        else if ((sensor_values.current_state == STANDBY) && !standby_state_written_to_LCD) {
            UG_FillFrame(10, 32, 30, 209, C_Highlight);
            LCD_PutStr(14, 41,  "S", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 63,  "T", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 85,  "A", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 107, "N", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 129, "D", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 151, "B", FONT_arial_20X23, C_Background, C_Highlight);
            LCD_PutStr(14, 173, "Y", FONT_arial_20X23, C_Background, C_Highlight);
            standby_state_written_to_LCD = 1;
            sleep_state_written_to_LCD = 0;
        }
        else if (sensor_values.current_state == RUN) {
            bar_top = 209 - (uint16_t)(filtered_power_percent * 177.0f);
            UG_FillFrame(10, bar_top, 30, 209,     C_PowerBar);
            UG_FillFrame(10, 32,      30, bar_top, C_Background);
            standby_state_written_to_LCD = 0;
            sleep_state_written_to_LCD = 0;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Main-screen layout (landscape fallback of the old text screen)     */
/* ------------------------------------------------------------------ */

static void LCD_draw_main_screen(void) {
    if ((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2)) {
        UG_FillScreen(C_Background);
        /* Opaque text: the partial updates below rely on it to wipe the
         * previous glyphs (menus/banner may have left transparency set). */
        UG_FontSetTransparency(0);

        /* No title bar: the station name line and its header rule ate the top
         * 40 px. The temperature blocks start right at the top instead. */
        LCD_PutStr(19, 8, "Set temp", FONT_arial_20X23, C_Text, C_Background);
        UG_DrawCircle(128, 39, 5, C_Text);
        UG_DrawCircle(128, 39, 4, C_Text);
        UG_DrawCircle(128, 39, 3, C_Text);
        if (flash_values.deg_celsius == 1) {
            LCD_PutStr(135, 33, "C", FONT_arial_36X44_C, C_Text, C_Background);
        } else {
            LCD_PutStr(135, 33, "F", FONT_arial_36X44_F, C_Text, C_Background);
        }
        LCD_PutStr(19, 92, "Actual temp", FONT_arial_20X23, C_Text, C_Background);
        UG_DrawCircle(128, 123, 5, C_Text);
        UG_DrawCircle(128, 123, 4, C_Text);
        UG_DrawCircle(128, 123, 3, C_Text);
        if (flash_values.deg_celsius == 1) {
            LCD_PutStr(135, 117, "C", FONT_arial_36X44_C, C_Text, C_Background);
        } else {
            LCD_PutStr(135, 117, "F", FONT_arial_36X44_F, C_Text, C_Background);
        }
        UG_DrawFrame(11, 86, 187, 168, C_Text);
        UG_DrawFrame(10, 85, 188, 169, C_Text);

        /* Handle / input voltage / power source are drawn by
         * display_app_tick(); only the static MCU label lives here. */
        if (flash_values.deg_celsius == 1) {
            LCD_PutStr(11, 275, "MCU:     °C", FONT_arial_17X18, C_Text, C_Background);
        } else {
            LCD_PutStr(11, 275, "MCU:      °F", FONT_arial_17X18, C_Text, C_Background);
        }

        if (flash_values.power_unit == 0) { // power unit is set to W
            LCD_PutStr(219, 275, "W", FONT_arial_17X18, C_Text, C_Background);
        } else {
            LCD_PutStr(219, 275, "%", FONT_arial_17X18, C_Text, C_Background);
        }

        UG_DrawLine(0, 296, 240, 296, C_Footer);
        UG_DrawLine(0, 297, 240, 297, C_Footer);

        if (flash_values.three_button_mode == 1) {
            LCD_PutStr(11, 301, "TEMP          UP   DOWN", FONT_arial_20X23, C_Footer, C_Background);
        } else {
            LCD_PutStr(11, 301, "PRESETS", FONT_arial_20X23, C_Footer, C_Background);
            memset(DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
            sprintf(DISPLAY_buffer, "%d", (int)convert_temperature(flash_values.preset_temp_1));
            LCD_PutStr(130, 301, DISPLAY_buffer, FONT_arial_20X23, C_Footer, C_Background);
            memset(DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
            sprintf(DISPLAY_buffer, "%d", (int)convert_temperature(flash_values.preset_temp_2));
            LCD_PutStr(190, 301, DISPLAY_buffer, FONT_arial_20X23, C_Footer, C_Background);
        }

        UG_DrawFrame(208, 64, 232, 270, C_Text);
        UG_DrawFrame(209, 65, 231, 269, C_Text);
    }
    else {
        UG_FillScreen(C_Background);
        UG_FontSetTransparency(0);

        LCD_PutStr(64, 10, "Set temp", FONT_arial_20X23, C_Text, C_Background);
        UG_DrawCircle(173, 41, 5, C_Text);
        UG_DrawCircle(173, 41, 4, C_Text);
        UG_DrawCircle(173, 41, 3, C_Text);
        if (flash_values.deg_celsius == 1) {
            LCD_PutStr(180, 35, "C", FONT_arial_36X44_C, C_Text, C_Background);
        } else {
            LCD_PutStr(180, 35, "F", FONT_arial_36X44_F, C_Text, C_Background);
        }
        LCD_PutStr(64, 90, "Actual temp", FONT_arial_20X23, C_Text, C_Background);
        UG_DrawCircle(173, 121, 5, C_Text);
        UG_DrawCircle(173, 121, 4, C_Text);
        UG_DrawCircle(173, 121, 3, C_Text);
        if (flash_values.deg_celsius == 1) {
            LCD_PutStr(180, 115, "C", FONT_arial_36X44_C, C_Text, C_Background);
        } else {
            LCD_PutStr(180, 115, "F", FONT_arial_36X44_F, C_Text, C_Background);
        }

        UG_DrawFrame(56, 84, 232, 170, C_Text);
        UG_DrawFrame(56, 83, 233, 171, C_Text);

        /* Handle / input voltage / power source are drawn by
         * display_app_tick(); only the static MCU label lives here. */
        if (flash_values.deg_celsius == 1) {
            LCD_PutStr(56, 215, "MCU:      °C", FONT_arial_17X18, C_Text, C_Background);
        } else {
            LCD_PutStr(56, 215, "MCU:       °F", FONT_arial_17X18, C_Text, C_Background);
        }

        UG_DrawLine(265, 0, 265, 240, C_Footer);
        UG_DrawLine(266, 0, 266, 240, C_Footer);
        UG_DrawLine(315, 0, 315, 240, C_Footer);
        UG_DrawLine(316, 0, 316, 240, C_Footer);

        if (flash_values.three_button_mode == 1) {
            LCD_PutStr(269, 90, "INC", FONT_arial_20X23, C_Footer, C_Background);
            LCD_PutStr(269, 10, "DEC", FONT_arial_20X23, C_Footer, C_Background);
        } else {
            LCD_PutStr(285, 75,  "P", FONT_arial_20X23, C_Footer, C_Background);
            LCD_PutStr(285, 97,  "R", FONT_arial_20X23, C_Footer, C_Background);
            LCD_PutStr(285, 119, "E", FONT_arial_20X23, C_Footer, C_Background);
            LCD_PutStr(285, 141, "S", FONT_arial_20X23, C_Footer, C_Background);
            LCD_PutStr(285, 163, "E", FONT_arial_20X23, C_Footer, C_Background);
            LCD_PutStr(285, 185, "T", FONT_arial_20X23, C_Footer, C_Background);
            LCD_PutStr(285, 207, "S", FONT_arial_20X23, C_Footer, C_Background);
            memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
            sprintf(DISPLAY_buffer, "%d", (int)flash_values.preset_temp_1);
            LCD_PutStr(272, 40, DISPLAY_buffer, FONT_arial_20X23, C_Footer, C_Background);
            memset(&DISPLAY_buffer, '\0', sizeof(DISPLAY_buffer));
            sprintf(DISPLAY_buffer, "%d", (int)flash_values.preset_temp_2);
            LCD_PutStr(272, 10, DISPLAY_buffer, FONT_arial_20X23, C_Footer, C_Background);
        }

        UG_DrawFrame(8, 30, 32, 212, C_Text);
        UG_DrawFrame(9, 31, 31, 211, C_Text);

        if (flash_values.power_unit == 0) { // power unit is set to W
            LCD_PutStr(37, 215, "W", FONT_arial_17X18, C_Text, C_Background);
        } else {
            LCD_PutStr(37, 215, "%", FONT_arial_17X18, C_Text, C_Background);
        }
    }

    /* Callers (buttons.c preset-save, show_popup) clear
     * sleep_state_written_to_LCD as needed to trigger overlay redraw. */
}

#endif /* !LCD_240X320 */

/* The JBC-style graph screen is the station's main screen; the gauge
 * dashboard is an alternative view of the same 240x320 station. */
static bool display_app_uses_graph_screen(void)
{
#if defined(LCD_240X320)
    return true;                 /* the JBC screen is the station's main UI */
#else
    return false;
#endif
}

/* Draw the static layout of the active screen (boot, popup dismissed,
 * settings/preset change). */
void display_app_draw_screen(void)
{
    display_banner_abort();
#if defined(LCD_240X320)
    if (alt_view) {
        alt_draw_static();
    } else {
        draw_graph_init();
    }
#else
    LCD_draw_main_screen();
#endif
}

/* ------------------------------------------------------------------ */
/* Popup overlay                                                      */
/* ------------------------------------------------------------------ */

void show_popup(char *text) {
    UG_FillFrame(10, 50, 235, 105, C_Highlight);
    UG_FillFrame(15, 55, 230, 100, C_Text);
    LCD_PutStr(20, 70, text, FONT_arial_20X23, C_Highlight, C_Text);
    popup_shown = 1;
    previous_millis_popup = HAL_GetTick();
    standby_state_written_to_LCD = 0;
    sleep_state_written_to_LCD = 0;
}

void LCD_draw_earth_fault_popup(void) {
    heater_off();
    sensor_values.requested_power = 0.0f;

    UG_FillFrame(10, 50, 205, 205, C_Highlight);
    UG_FillFrame(15, 55, 200, 200, C_Text);
    LCD_PutStr(20, 60,  "GROUNDING",   FONT_arial_20X23, C_Highlight, C_Text);
    LCD_PutStr(20, 80,  "ERROR",       FONT_arial_20X23, C_Highlight, C_Text);

    LCD_PutStr(20, 120, "CHECK",       FONT_arial_20X23, C_Highlight, C_Text);
    LCD_PutStr(20, 140, "CONNECTIONS", FONT_arial_20X23, C_Highlight, C_Text);
    LCD_PutStr(20, 160, "AND REBOOT",  FONT_arial_20X23, C_Highlight, C_Text);

    Error_Handler();
}

/* ------------------------------------------------------------------ */
/* USB-PD source banner (boot)                                        */
/* ------------------------------------------------------------------ */

/* The two screens below are diagnostics: they are compiled into STUSB_DEBUG
 * builds only (tools/build.sh --debug), so a release image carries neither the
 * strings nor the code. */

#ifdef STUSB_DEBUG

/* ---- NVM settings dump ----
 *
 * The controller negotiates its first contract from its own NVM before the MCU
 * is up, so these bytes explain what a source sees at plug-in. Layout follows
 * ST's NVM flasher: sector 3 holds the sink PDO count and the PDO currents,
 * sector 4 the PDO2/PDO3 voltages. Currents are encoded as 0 = 0 A,
 * 1..10 = code x 0.25 A + 0.25 A, 11..15 = code x 0.5 A - 2.5 A.
 */
static uint16_t nvm_current_ma(uint8_t code)
{
    if (code == 0u) {
        return 0u;
    }
    if (code < 11u) {
        return (uint16_t)((code * 250u) + 250u);
    }
    return (uint16_t)((code * 500u) - 2500u);
}

void LCD_show_pd_apply(float req_v, float req_a, float vbus, uint32_t rdo_raw,
                       uint8_t rdo_pos, float rdo_max_a, float rdo_oper_a,
                       float contract, float limit, const stusb_dpm_trace_t *trace)
{
    char     line[40];
    uint16_t y = 34;

    UG_FillScreen(C_Background);
    UG_FontSetTransparency(0);

    LCD_PutStr(8, 8, "PD CONTRACT", FONT_arial_17X18, C_Header, C_Background);

    (void)snprintf(line, sizeof(line), "req  %u.%02uV %u.%02uA",
                   (unsigned)(int)req_v, (unsigned)((int)(req_v * 100.0f) % 100),
                   (unsigned)(int)req_a, (unsigned)((int)(req_a * 100.0f) % 100));
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Text, C_Background);

    y = (uint16_t)(y + 20u);
    if (trace != 0) {
        (void)snprintf(line, sizeof(line), "dpm  %u %08lX",
                       (unsigned)trace->count_readback, (unsigned long)trace->pdo_readback);
        LCD_PutStr(8, y, line, FONT_arial_12X15, C_Warning, C_Background);

        y = (uint16_t)(y + 20u);
        (void)snprintf(line, sizeof(line), "post %u %08lX",
                       (unsigned)trace->count_after, (unsigned long)trace->pdo_after);
        LCD_PutStr(8, y, line, FONT_arial_12X15, C_Warning, C_Background);
    }

    y = (uint16_t)(y + 20u);
    (void)snprintf(line, sizeof(line), "vbus %u.%02uV",
                   (unsigned)(int)vbus, (unsigned)((int)(vbus * 100.0f) % 100));
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Text, C_Background);

    y = (uint16_t)(y + 20u);
    (void)snprintf(line, sizeof(line), "rdo  %08lX", (unsigned long)rdo_raw);
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Footer, C_Background);

    y = (uint16_t)(y + 20u);
    (void)snprintf(line, sizeof(line), "pos %u %u.%02u/%u.%02uA",
                   (unsigned)rdo_pos,
                   (unsigned)(int)rdo_max_a, (unsigned)((int)(rdo_max_a * 100.0f) % 100),
                   (unsigned)(int)rdo_oper_a, (unsigned)((int)(rdo_oper_a * 100.0f) % 100));
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Text, C_Background);

    y = (uint16_t)(y + 20u);
    (void)snprintf(line, sizeof(line), "%uW tip %uW",
                   (unsigned)(int)(contract + 0.5f), (unsigned)(int)(limit + 0.5f));
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Header, C_Background);
}

void LCD_show_nvm_dump(const uint8_t *nvm)
{
    const uint8_t *s3 = &nvm[3u * STUSB_NVM_SECTOR_SIZE];
    const uint8_t *s4 = &nvm[4u * STUSB_NVM_SECTOR_SIZE];
    uint16_t v2_mv = (uint16_t)(((((uint16_t)s4[1] << 8) | s4[0]) >> 6) * 50u);
    uint16_t v3_mv = (uint16_t)(((((uint16_t)s4[3] & 0x03u) << 8) | s4[2]) * 50u);
    uint16_t i1_ma = nvm_current_ma((uint8_t)((s3[2] & 0xF0u) >> 4));
    uint16_t i2_ma = nvm_current_ma((uint8_t)(s3[4] & 0x0Fu));
    uint16_t i3_ma = nvm_current_ma((uint8_t)((s3[5] & 0xF0u) >> 4));
    uint16_t y = 56;
    char     line[40];

    UG_FillScreen(C_Background);
    UG_FontSetTransparency(0);

    LCD_PutStr(8, 8, "STUSB NVM", FONT_arial_17X18, C_Header, C_Background);

    (void)snprintf(line, sizeof(line), "pdos %u", (unsigned)((s3[2] & 0x06u) >> 1));
    LCD_PutStr(8, 34, line, FONT_arial_12X15, C_Text, C_Background);

    (void)snprintf(line, sizeof(line), "1:5.00V %u.%02uA",
                   (unsigned)(i1_ma / 1000u), (unsigned)((i1_ma % 1000u) / 10u));
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Text, C_Background);

    y = (uint16_t)(y + 20u);
    (void)snprintf(line, sizeof(line), "2:%u.%02uV %u.%02uA",
                   (unsigned)(v2_mv / 1000u), (unsigned)((v2_mv % 1000u) / 10u),
                   (unsigned)(i2_ma / 1000u), (unsigned)((i2_ma % 1000u) / 10u));
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Text, C_Background);

    y = (uint16_t)(y + 20u);
    (void)snprintf(line, sizeof(line), "3:%u.%02uV %u.%02uA",
                   (unsigned)(v3_mv / 1000u), (unsigned)((v3_mv % 1000u) / 10u),
                   (unsigned)(i3_ma / 1000u), (unsigned)((i3_ma % 1000u) / 10u));
    LCD_PutStr(8, y, line, FONT_arial_12X15, C_Text, C_Background);
}

#endif /* STUSB_DEBUG */

/* ---- USB-PD boot banner / mode chooser ---- */

/* First text line of the list, per orientation. */
static uint16_t pd_banner_list_y(void)
{
    return (uint16_t)(((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2)) ? 40 : 36);
}

/* Draw one entry of the accepted-profile list; `pos` is its position in the
 * list, `active` marks the cursor. Only two of these are redrawn when the user
 * turns the knob, which keeps the display from flickering. */
static void draw_pd_banner_line(const uint8_t *list, uint8_t pos, uint8_t active)
{
    const pd_offer_t *o = &pd_offers[list[pos]];
    uint32_t watts = (((uint32_t)o->mv * o->ma) + 500000u) / 1000000u;
    char     line[32];

    (void)snprintf(line, sizeof(line), "%c %2u.%uV %u.%02uA %3uW",
                   active ? '>' : ' ',
                   (unsigned)(o->mv / 1000u), (unsigned)((o->mv % 1000u) / 100u),
                   (unsigned)(o->ma / 1000u), (unsigned)((o->ma % 1000u) / 10u),
                   (unsigned)watts);

    LCD_PutStr(8, (uint16_t)(pd_banner_list_y() + (pos * 16u)), line, FONT_arial_12X15,
               active ? C_Header : C_Text, C_Background);
}

/* Draw the whole chooser. `list` holds the pd_offers[] indices of the accepted
 * profiles, `sel` is the cursor position inside that list. */
static void draw_pd_banner_body(const uint8_t *list, uint8_t count, uint8_t sel)
{
    uint16_t y = pd_banner_list_y();
    uint8_t  portrait = (uint8_t)((flash_values.screen_rotation == 0) || (flash_values.screen_rotation == 2));

    UG_FillScreen(C_Background);

    LCD_PutStr(8, 8, "USB-PD SOURCE", FONT_arial_17X18, C_Header, C_Background);

    if (count == 0u) {
        LCD_PutStr(8, y, "no modes offered", FONT_arial_12X15, C_Warning, C_Background);
        LCD_PutStr(8, (uint16_t)(y + 16), (pd_status == PD_STATUS_NO_CAPS)
                                            ? "(capabilities unreadable)"
                                            : "(5 V source or no PD)",
                   FONT_arial_12X15, C_Footer, C_Background);
    }

    for (uint8_t i = 0; i < count; i++) {
        draw_pd_banner_line(list, i, (uint8_t)(i == sel));
    }

    /* Bottom hint bar: knob selects, any button confirms. */
    LCD_PutStr(8, (uint16_t)(portrait ? 292 : 212), "turn=select  press=OK",
               FONT_arial_12X15, C_Footer, C_Background);
}

/* Boot-time USB-PD mode chooser.
 *
 * Shows what the source advertised (collected by power_source_init(), whatever
 * voltage the controller negotiated on its own) and lets the user pick one with
 * the rotary encoder; any button confirms. Returns the index into pd_offers[] of
 * the mode that was confirmed, or -1 when the banner closed without a choice
 * (a button was not pressed within PD_BANNER_TIMEOUT_MS) or the source offered
 * nothing. main() then applies pd_offer_default, so dismissing the window never
 * looks like a user selection. */
int8_t LCD_run_pd_banner(void)
{
    uint8_t list[PD_OFFER_MAX];
    uint8_t count = 0;
    uint8_t sel   = 0;

    /* Collect the accepted profiles, highest power first. */
    for (uint8_t i = 0; i < pd_offer_count; i++) {
        if (!pd_offers[i].accepted) {
            continue;
        }
        if ((uint8_t)i == pd_offer_default) {
            sel = count;                 /* start on the mode that will be used */
        }
        list[count++] = i;
    }

    /* Opaque text: the main screen relies on transparency 0 to wipe glyphs. */
    uint8_t transparency = UG_FontGetTransparency();
    UG_FontSetTransparency(0);

    (void)encoder_read_detents();        /* sync, drop stale movement */
    draw_pd_banner_body(list, count, sel);

    uint32_t last_activity = HAL_GetTick();
    bool     confirmed     = false;

    for (;;) {
        /* In 3-button mode TIM2->CNT *is* the set temperature (driven by the
         * preset buttons), so there is no knob to read. */
        int16_t detents = (flash_values.three_button_mode == 1) ? 0 : encoder_read_detents();

        /* Turning the knob only MOVES the cursor - it never selects anything. */
        if ((detents != 0) && (count > 1u)) {
            int16_t next = (int16_t)sel + detents;
            if (next < 0) { next = 0; }
            if (next > (int16_t)(count - 1u)) { next = (int16_t)(count - 1u); }
            if ((uint8_t)next != sel) {
                uint8_t previous = sel;
                sel = (uint8_t)next;
                draw_pd_banner_line(list, previous, 0);
                draw_pd_banner_line(list, sel, 1);
            }
            last_activity = HAL_GetTick();
        }

        /* Only a button press confirms the highlighted mode. */
        if (buttons_take_press(BTN_1) || buttons_take_press(BTN_2) ||
            buttons_take_press(BTN_3)) {
            confirmed = true;
            break;
        }

        if ((HAL_GetTick() - last_activity) > PD_BANNER_TIMEOUT_MS) {
            break;                       /* idle: keep the boot-time selection */
        }

        HAL_Delay(20);
    }

    UG_FontSetTransparency(transparency);

    if (!confirmed) {
        return -1;                       /* dismissed: main() applies the default */
    }

    return (count > 0u) ? (int8_t)list[sel] : -1;
}

/* ------------------------------------------------------------------ */
/* Tick                                                               */
/* ------------------------------------------------------------------ */

void display_app_tick(void){
	if(popup_shown == 1){
		display_banner_abort();
		if(HAL_GetTick() - previous_millis_popup >= POPUP_DWELL_MS){
			popup_shown = 0;

			/* Clear the area where the emergency message was displayed. */
			display_app_draw_screen();
		}
	}
	else{
		/* Update display */
		if(HAL_GetTick() - previous_millis_display >= DISPLAY_TICK_INTERVAL_MS){
			sensor_values.requested_power_filtered = clamp(Moving_Average_Compute(sensor_values.requested_power, &requested_power_filtered_filter_struct), 0.0f, PID_MAX_OUTPUT);

#if defined(LCD_240X320)
			if (display_app_uses_graph_screen()){
				static uint8_t graph_decim = 0;

				if (++graph_decim >= graph_timebase_decim()) {
					/* Store the actual heater power in 0.1 W (the graph axis is
					 * in watts): requested PID output scaled by the budget. */
					float max_w = sensor_values.max_power_watt;
					float graf_power;

					if (max_w < 1.0f) {
						max_w = 1.0f;
					}
					graf_power = sensor_values.requested_power * (max_w * 10.0f / PID_MAX_OUTPUT);

					if (graf_power > max_w * 10.0f) {
						graf_power = max_w * 10.0f;
					}
					graph_decim = 0;
					add_data_point((uint16_t) convert_temperature(sensor_values.thermocouple_temperature_filtered),
					               (uint16_t) graf_power,
					               (uint16_t) sensor_values.set_temperature);
				}
			}
			display_banner_tick();
			if (!display_banner_active()) {
				if (display_app_uses_graph_screen()){
					if (alt_view) {
						alt_update_display();
					} else {
						draw_graph_update();
						update_graph_display();
					}
				}
			}
#else
			{
				update_display();
			}
#endif

			previous_millis_display = HAL_GetTick();
		}
	}
}
