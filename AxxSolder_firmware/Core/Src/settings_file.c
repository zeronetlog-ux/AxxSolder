#include "settings_file.h"
#include "settings.h"
#include "main.h"        /* MAX_POWER, MAX_SELECTABLE_TEMPERATURE */
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Field description: file key, offset inside Flash_values and allowed range.
 * Ranges mirror normalize_param() in menu_settings.c so a hand-edited file
 * cannot put the controller into a state the menu could never reach. */
typedef struct {
    const char *key;
    uint16_t    offset;
    float       min;
    float       max;
} SettingsFileField;

#define F(field, lo, hi) { #field, offsetof(Flash_values, field), (lo), (hi) }

static const SettingsFileField field_table[] = {
    F(startup_temperature,           0.0f,   MAX_SELECTABLE_TEMPERATURE),
    F(temperature_offset,          -50.0f,   50.0f),
    F(standby_temp,                  0.0f,   MAX_SELECTABLE_TEMPERATURE),
    F(standby_time,                  0.0f,   60.0f),
    F(emergency_time,                0.0f,  120.0f),
    F(buzzer_enabled,                0.0f,    1.0f),
    F(preset_temp_1,                 0.0f,   MAX_SELECTABLE_TEMPERATURE),
    F(preset_temp_2,                 0.0f,   MAX_SELECTABLE_TEMPERATURE),
    F(GPIO4_ON_at_run,               0.0f,    1.0f),
    F(screen_rotation,               0.0f,    3.0f),
    F(momentary_stand,               0.0f,    1.0f),
    F(current_measurement,           0.0f,    1.0f),
    F(startup_beep,                  0.0f,    1.0f),
    F(deg_celsius,                   0.0f,    1.0f),
    F(temp_cal_100,                  0.0f,  600.0f),
    F(temp_cal_200,                  0.0f,  600.0f),
    F(temp_cal_300,                  0.0f,  600.0f),
    F(temp_cal_350,                  0.0f,  600.0f),
    F(temp_cal_400,                  0.0f,  600.0f),
    F(temp_cal_450,                  0.0f,  600.0f),
    F(serial_debug_print,            0.0f,    1.0f),
    F(displayed_temp_filter,         1.0f,   10.0f),
    F(startup_temp_is_previous_temp, 0.0f,    1.0f),
    F(three_button_mode,             0.0f,    1.0f),
    F(beep_at_set_temp,              0.0f,    1.0f),
    F(beep_tone,                     0.0f,    3.0f),
    F(power_unit,                    0.0f,    1.0f),
    F(detect_nt115,                  0.0f,    1.0f),
    F(power_limit_T245,              0.0f,   MAX_POWER),
    F(power_limit_T210,              0.0f,   MAX_POWER),
    F(power_limit_NT115,             0.0f,   MAX_POWER),
    F(power_limit_No_name,           0.0f,   MAX_POWER),
    F(display_graph,                 0.0f,    1.0f),
    F(delta_t_detection,             0.0f,    1.0f),
    F(standby_delay,                 0.0f,   60.0f),
    F(show_profile_on_tip_change,    0.0f,    1.0f),
    F(change_enc_dir,                0.0f,    1.0f),
    F(encoder_step_idx,              0.0f,    3.0f),
    F(heat_at_startup,               0.0f,    1.0f),
    F(pd_profile,                   -1.0f,   21.0f),  /* -1 = auto, else volts */
    F(pd_auto_max,                   0.0f,    1.0f),  /* 1 = skip the chooser */
};

#define FIELD_COUNT (sizeof(field_table) / sizeof(field_table[0]))

/* Guard: every Flash_values field must have a key in the file. */
_Static_assert(FIELD_COUNT == sizeof(Flash_values) / sizeof(float),
               "AXX.CFG is missing a Flash_values field");

/* Set when a parsed value differs from the current one - avoids rewriting the
 * settings page when the host only rewrites the same content. */
static bool settings_changed;

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/* Float -> "[-]123.45". printf("%f") is not available: the project links with
 * --specs=nano.specs and without -u _printf_float. */
void settings_file_fmt_float(char *out, uint16_t len, float value)
{
    bool     negative = (value < 0.0f);
    float    absolute = negative ? -value : value;
    uint32_t whole    = (uint32_t)absolute;
    uint32_t fraction = (uint32_t)((absolute - (float)whole) * 100.0f + 0.5f);

    if (fraction >= 100u) {
        whole++;
        fraction -= 100u;
    }

    (void)snprintf(out, len, "%s%lu.%02lu",
                   negative ? "-" : "",
                   (unsigned long)whole,
                   (unsigned long)fraction);
}

static float clamp_value(float value, float min, float max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

static const SettingsFileField *find_field(const char *key, size_t key_len)
{
    for (size_t i = 0; i < FIELD_COUNT; i++) {
        if ((strlen(field_table[i].key) == key_len) &&
            (strncmp(field_table[i].key, key, key_len) == 0)) {
            return &field_table[i];
        }
    }
    return NULL;
}

static const char *skip_spaces(const char *s)
{
    while ((*s == ' ') || (*s == '\t')) {
        s++;
    }
    return s;
}

/* Minimal unsigned parser - avoids pulling strtoul/strtod into the image. */
bool settings_file_parse_uint(const char *s, unsigned long *out)
{
    unsigned long value = 0;
    bool          any   = false;

    while ((*s >= '0') && (*s <= '9')) {
        value = (value * 10UL) + (unsigned long)(*s - '0');
        s++;
        any = true;
    }

    *out = value;
    return any;
}

/* Minimal float parser: [-]digits[.digits] - the settings file never contains
 * exponents and strtof() would add several kilobytes of soft-float code. */
bool settings_file_parse_float(const char *s, float *out)
{
    bool     negative = false;
    float    value    = 0.0f;
    float    scale    = 0.1f;
    bool     any      = false;

    if (*s == '-') {
        negative = true;
        s++;
    } else if (*s == '+') {
        s++;
    }

    while ((*s >= '0') && (*s <= '9')) {
        value = (value * 10.0f) + (float)(*s - '0');
        s++;
        any = true;
    }

    if (*s == '.') {
        s++;
        while ((*s >= '0') && (*s <= '9')) {
            value += (float)(*s - '0') * scale;
            scale *= 0.1f;
            s++;
            any = true;
        }
    }

    *out = negative ? -value : value;
    return any;
}

/* Parse one "key=value" line. Returns 1 if a setting was applied, 0 if the
 * line was ignored (comment/unknown key) and -1 on a too-new file version. */
static int8_t apply_line(char *line, uint16_t len, uint8_t *version)
{
    char       *eq;
    size_t      key_len;
    const char *value_str;

    /* Trim trailing spaces/CR */
    while ((len > 0) && ((line[len - 1] == ' ') || (line[len - 1] == '\r') ||
                         (line[len - 1] == '\t'))) {
        len--;
    }
    line[len] = '\0';

    char *start = (char *)skip_spaces(line);
    if ((*start == '\0') || (*start == '#') || (*start == ';')) {
        return 0;
    }

    eq = strchr(start, '=');
    if (eq == NULL) {
        return 0;
    }

    key_len = (size_t)(eq - start);
    while ((key_len > 0) && ((start[key_len - 1] == ' ') || (start[key_len - 1] == '\t'))) {
        key_len--;
    }
    value_str = skip_spaces(eq + 1);

    if ((key_len == 7) && (strncmp(start, "version", 7) == 0)) {
        unsigned long file_version = 0;

        if (!settings_file_parse_uint(value_str, &file_version) || (file_version > SETTINGS_FILE_VERSION)) {
            return -1;
        }
        *version = (uint8_t)file_version;
        return 0;
    }

    const SettingsFileField *field = find_field(start, key_len);
    if (field == NULL) {
        return 0;   /* unknown key: ignore (file may come from a newer build) */
    }

    float parsed = 0.0f;
    if (!settings_file_parse_float(value_str, &parsed)) {
        return 0;   /* not a number */
    }

    float *slot = (float *)((uint8_t *)&flash_values + field->offset);
    float  value = clamp_value(parsed, field->min, field->max);

    if (*slot != value) {
        *slot = value;
        settings_changed = true;
    }

    return 1;
}

/* ------------------------------------------------------------------ */
/* Export                                                             */
/* ------------------------------------------------------------------ */

FRESULT settings_file_export(void)
{
    static const char header[] =
        "# AxxSolder settings\r\n"
        "# key=value, one per line, '#' starts a comment\r\n"
        "# Unplug power from the soldering iron to leave USB-storage mode.\r\n"
        "version=1\r\n";

    FIL     file;
    UINT    written;
    FRESULT res = f_open(&file, SETTINGS_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);

    if (res != FR_OK) {
        return res;
    }

    res = f_write(&file, header, (UINT)(sizeof(header) - 1), &written);

    for (size_t i = 0; (res == FR_OK) && (i < FIELD_COUNT); i++) {
        char     line[64];
        float    value   = *(float *)((uint8_t *)&flash_values + field_table[i].offset);
        size_t   key_len = strlen(field_table[i].key);

        memcpy(line, field_table[i].key, key_len);
        line[key_len] = '=';
        settings_file_fmt_float(&line[key_len + 1], (uint16_t)(sizeof(line) - key_len - 1), value);

        size_t line_len = strlen(line);
        line[line_len++] = '\r';
        line[line_len++] = '\n';

        res = f_write(&file, line, (UINT)line_len, &written);
    }

    FRESULT close_res = f_close(&file);
    return (res == FR_OK) ? close_res : res;
}

/* ------------------------------------------------------------------ */
/* Import                                                             */
/* ------------------------------------------------------------------ */

int16_t settings_file_import(void)
{
    FIL      file;
    FRESULT  res = f_open(&file, SETTINGS_FILE_PATH, FA_READ);
    uint8_t  chunk[128];
    char     line[96];
    uint16_t line_len = 0;
    uint16_t applied  = 0;
    uint8_t  version  = 0;
    int8_t   status   = 0;

    if (res != FR_OK) {
        return -1;
    }

    settings_changed = false;

    for (;;) {
        UINT received = 0;

        res = f_read(&file, chunk, (UINT)sizeof(chunk), &received);
        if ((res != FR_OK) || (received == 0)) {
            break;
        }

        for (UINT i = 0; i < received; i++) {
            char c = (char)chunk[i];

            if ((c == '\n') || (c == '\r')) {
                if (line_len > 0) {
                    status = apply_line(line, line_len, &version);
                    if (status > 0) {
                        applied++;
                    }
                    line_len = 0;
                    if (status < 0) {
                        break;          /* too-new format: stop parsing */
                    }
                }
            } else if (line_len < (sizeof(line) - 1)) {
                line[line_len++] = c;
            }
        }

        if (status < 0) {
            break;
        }
    }

    (void)f_close(&file);

    if (status < 0) {
        return -1;
    }

    if ((applied > 0) && settings_changed) {
        settings_save();
    }

    return (int16_t)applied;
}
