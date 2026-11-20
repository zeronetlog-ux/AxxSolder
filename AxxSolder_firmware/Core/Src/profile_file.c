#include "profile_file.h"
#include "settings_file.h"     /* shared number helpers */
#include "tip_profile.h"
#include "storage.h"
#include "main.h"              /* MAX_POWER */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define ACTIVE_FILE          "/ACTIVE.CFG"
#define PROFILE_FILE_VERSION 1

/* Calibration points, index order matches TipProfile.temp_cal[]. */
static const uint16_t cal_points[NUM_CAL_POINTS] = { 100u, 200u, 300u, 350u, 400u, 450u };

static const char *const handle_names[] = { "NT115", "T210", "T245", "No_name" };

/* Keys of ACTIVE.CFG, index order matches the enum handles. */
static const char *const active_keys[] = { "nt115", "t210", "t245", "no_name" };

static const enum handles handle_list[4] = { NT115, T210, T245, No_name };

static float clampf(float value, float min, float max)
{
    if (value < min) {
        return min;
    }
    if (value > max) {
        return max;
    }
    return value;
}

/* ------------------------------------------------------------------ */
/* Small helpers                                                      */
/* ------------------------------------------------------------------ */

static void profile_path(char *out, uint16_t len, uint8_t index)
{
    (void)snprintf(out, len, "/P%02u.CFG", (unsigned)(index + 1u));
}

static FRESULT put_str(FIL *f, const char *s)
{
    UINT written;
    return f_write(f, s, (UINT)strlen(s), &written);
}

static FRESULT put_float(FIL *f, const char *key, float value)
{
    char   line[48];
    size_t key_len = strlen(key);

    memcpy(line, key, key_len);
    line[key_len] = '=';
    settings_file_fmt_float(&line[key_len + 1], (uint16_t)(sizeof(line) - key_len - 3), value);

    size_t len = strlen(line);
    line[len++] = '\r';
    line[len++] = '\n';

    UINT written;
    return f_write(f, line, (UINT)len, &written);
}

static FRESULT put_text(FIL *f, const char *key, const char *value)
{
    char line[64];

    (void)snprintf(line, sizeof(line), "%s=%s\r\n", key, value);
    return put_str(f, line);
}

/* ------------------------------------------------------------------ */
/* Export                                                             */
/* ------------------------------------------------------------------ */

FRESULT profile_files_export(void)
{
    static const char header[] =
        "# AxxSolder tip profile\r\n"
        "# key=value, '#' starts a comment, version=1\r\n"
        "version=1\r\n";

    FIL     f;
    FRESULT res   = FR_OK;
    uint8_t count = tip_profiles_count();
    char    path[16];

    for (uint8_t i = 0; (res == FR_OK) && (i < count); i++) {
        const TipProfile *p = tip_profiles_get(i);

        if (p == NULL) {
            continue;
        }

        profile_path(path, sizeof(path), i);
        res = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
        if (res != FR_OK) {
            break;
        }

        res = put_str(&f, header);

        if (res == FR_OK) {
            char name[PROFILE_NAME_LEN + 1];

            memcpy(name, p->name, PROFILE_NAME_LEN);
            name[PROFILE_NAME_LEN] = '\0';
            res = put_text(&f, "name", name);
        }
        if (res == FR_OK) {
            uint8_t handle = (p->handle_type < 4u) ? p->handle_type : 3u;
            res = put_text(&f, "handle", handle_names[handle]);
        }
        if (res == FR_OK) res = put_float(&f, "default", (float)p->def);
        if (res == FR_OK) res = put_float(&f, "kp", p->kp);
        if (res == FR_OK) res = put_float(&f, "ki", p->ki);
        if (res == FR_OK) res = put_float(&f, "kd", p->kd);
        if (res == FR_OK) res = put_float(&f, "max_i", p->max_i);
        if (res == FR_OK) res = put_float(&f, "power_limit", p->power_limit);

        for (uint8_t c = 0; (res == FR_OK) && (c < NUM_CAL_POINTS); c++) {
            char key[16];

            (void)snprintf(key, sizeof(key), "temp_cal_%u", (unsigned)cal_points[c]);
            res = put_float(&f, key, p->temp_cal[c]);
        }

        FRESULT close_res = f_close(&f);
        if (res == FR_OK) {
            res = close_res;
        }
    }

    /* Active profile per handle, in its own small file. */
    if (res == FR_OK) {
        res = f_open(&f, ACTIVE_FILE, FA_CREATE_ALWAYS | FA_WRITE);
    }
    if (res == FR_OK) {
        res = put_str(&f, "# active profile index per handle, -1 = none\r\nversion=1\r\n");

        for (uint8_t h = 0; (res == FR_OK) && (h < 4u); h++) {
            uint8_t idx = tip_profiles_get_active(handle_list[h]);
            char    value[8];

            if (idx == 0xFFu) {
                (void)strcpy(value, "-1");
            } else {
                (void)snprintf(value, sizeof(value), "%u", (unsigned)idx);
            }
            res = put_text(&f, active_keys[h], value);
        }

        FRESULT close_res = f_close(&f);
        if (res == FR_OK) {
            res = close_res;
        }
    }

    /* Invalidate the first unused slot by writing a header-only file: the
     * import stops at the first file without keys, so no f_unlink() is needed
     * (which keeps that code out of the image). */
    if ((res == FR_OK) && (count < MAX_PROFILES)) {
        profile_path(path, sizeof(path), count);

        res = f_open(&f, path, FA_CREATE_ALWAYS | FA_WRITE);
        if (res == FR_OK) {
            res = put_str(&f, header);

            FRESULT close_res = f_close(&f);
            if (res == FR_OK) {
                res = close_res;
            }
        }
    }

    return res;
}

/* ------------------------------------------------------------------ */
/* Import                                                             */
/* ------------------------------------------------------------------ */

/* Apply one "key=value" line to the profile.
 * Returns 1 if applied, 0 if ignored and -1 on a too-new format version. */
static int8_t apply_profile_line(char *line, uint16_t len, TipProfile *p)
{
    while ((len > 0) && ((line[len - 1] == ' ') || (line[len - 1] == '\r') ||
                         (line[len - 1] == '\t'))) {
        len--;
    }
    line[len] = '\0';

    char *start = line;
    while ((*start == ' ') || (*start == '\t')) {
        start++;
    }
    if ((*start == '\0') || (*start == '#') || (*start == ';')) {
        return 0;
    }

    char *eq = strchr(start, '=');
    if (eq == NULL) {
        return 0;
    }

    size_t key_len = (size_t)(eq - start);
    while ((key_len > 0) && ((start[key_len - 1] == ' ') || (start[key_len - 1] == '\t'))) {
        key_len--;
    }

    const char *value = eq + 1;
    while ((*value == ' ') || (*value == '\t')) {
        value++;
    }

    if ((key_len == 7) && (strncmp(start, "version", 7) == 0)) {
        unsigned long version = 0;

        if (!settings_file_parse_uint(value, &version) || (version > PROFILE_FILE_VERSION)) {
            return -1;
        }
        return 0;
    }

    if ((key_len == 4) && (strncmp(start, "name", 4) == 0)) {
        size_t n = strlen(value);

        if (n > PROFILE_NAME_LEN) {
            n = PROFILE_NAME_LEN;
        }
        memset(p->name, 0, PROFILE_NAME_LEN);
        memcpy(p->name, value, n);
        return 1;
    }

    if ((key_len == 6) && (strncmp(start, "handle", 6) == 0)) {
        for (uint8_t h = 0; h < 4u; h++) {
            if (strcmp(value, handle_names[h]) == 0) {
                p->handle_type = h;
                return 1;
            }
        }
        return 0;
    }

    if ((key_len > 9) && (strncmp(start, "temp_cal_", 9) == 0)) {
        unsigned long point = 0;
        float         v     = 0.0f;

        if (!settings_file_parse_uint(start + 9, &point) ||
            !settings_file_parse_float(value, &v)) {
            return 0;
        }
        for (uint8_t c = 0; c < NUM_CAL_POINTS; c++) {
            if (cal_points[c] == point) {
                p->temp_cal[c] = clampf(v, 0.0f, 600.0f);
                return 1;
            }
        }
        return 0;
    }

    float v = 0.0f;
    if (!settings_file_parse_float(value, &v)) {
        return 0;
    }

    if ((key_len == 7) && (strncmp(start, "default", 7) == 0)) {
        p->def = (uint8_t)(v != 0.0f);
    } else if ((key_len == 2) && (strncmp(start, "kp", 2) == 0)) {
        p->kp = clampf(v, 0.0f, 100.0f);
    } else if ((key_len == 2) && (strncmp(start, "ki", 2) == 0)) {
        p->ki = clampf(v, 0.0f, 100.0f);
    } else if ((key_len == 2) && (strncmp(start, "kd", 2) == 0)) {
        p->kd = clampf(v, 0.0f, 100.0f);
    } else if ((key_len == 5) && (strncmp(start, "max_i", 5) == 0)) {
        p->max_i = clampf(v, 0.0f, 1000.0f);
    } else if ((key_len == 11) && (strncmp(start, "power_limit", 11) == 0)) {
        p->power_limit = clampf(v, 0.0f, MAX_POWER);
    } else {
        return 0;
    }

    return 1;
}

/* Read one profile file; returns true if at least one key was applied. */
static bool parse_profile_file(FIL *f, TipProfile *p)
{
    uint8_t  chunk[64];
    char     line[96];
    uint16_t line_len = 0;
    bool     applied  = false;

    memset(p, 0, sizeof(*p));

    for (;;) {
        UINT    got = 0;
        FRESULT res = f_read(f, chunk, (UINT)sizeof(chunk), &got);

        if ((res != FR_OK) || (got == 0)) {
            break;
        }

        for (UINT i = 0; i < got; i++) {
            char c = (char)chunk[i];

            if ((c == '\n') || (c == '\r')) {
                if (line_len > 0) {
                    int8_t r = apply_profile_line(line, line_len, p);

                    line_len = 0;
                    if (r < 0) {
                        return false;
                    }
                    if (r > 0) {
                        applied = true;
                    }
                }
            } else if (line_len < (sizeof(line) - 1)) {
                line[line_len++] = c;
            }
        }
    }

    if (line_len > 0) {
        int8_t r = apply_profile_line(line, line_len, p);

        if (r < 0) {
            return false;
        }
        if (r > 0) {
            applied = true;
        }
    }

    return applied;
}

/* Apply one line of ACTIVE.CFG: "<handle>=<index>" ("-1" leaves it unset). */
static void apply_active_line(char *line, uint16_t len)
{
    unsigned long index = 0;

    line[len] = '\0';

    char *eq = strchr(line, '=');
    if (eq == NULL) {
        return;
    }

    size_t key_len = (size_t)(eq - line);

    if (!settings_file_parse_uint(eq + 1, &index)) {
        return;
    }

    for (uint8_t h = 0; h < 4u; h++) {
        if ((strlen(active_keys[h]) == key_len) &&
            (strncmp(active_keys[h], line, key_len) == 0)) {
            tip_profiles_set_active(handle_list[h],
                                    (index <= 0xFEUL) ? (uint8_t)index : 0xFFu);
            return;
        }
    }
}

/* Apply the per-handle active indexes (values are validated by tip_profile). */
static void import_active(void)
{
    FIL      f;
    uint8_t  chunk[32];
    char     line[32];
    uint16_t line_len = 0;

    if (f_open(&f, ACTIVE_FILE, FA_READ) != FR_OK) {
        return;
    }

    for (;;) {
        UINT    got = 0;
        FRESULT res = f_read(&f, chunk, (UINT)sizeof(chunk), &got);

        if ((res != FR_OK) || (got == 0)) {
            break;
        }

        for (UINT i = 0; i < got; i++) {
            char c = (char)chunk[i];

            if ((c == '\n') || (c == '\r')) {
                if (line_len > 0) {
                    apply_active_line(line, line_len);
                    line_len = 0;
                }
            } else if (line_len < (sizeof(line) - 1)) {
                line[line_len++] = c;
            }
        }
    }

    if (line_len > 0) {
        apply_active_line(line, line_len);
    }

    (void)f_close(&f);
}

int16_t profile_files_import(void)
{
    FIL         f;
    TipProfile  profile;
    char        path[16];
    uint8_t     count = 0;

    tip_profiles_clear();

    /* The files are written as P01, P02, ... with no holes: stop at the first
     * one that is missing or invalid. */
    for (uint8_t i = 0; i < MAX_PROFILES; i++) {
        profile_path(path, sizeof(path), i);

        if (f_open(&f, path, FA_READ) != FR_OK) {
            break;
        }

        bool ok = parse_profile_file(&f, &profile);
        (void)f_close(&f);

        if (!ok || (tip_profiles_add(&profile) != STORE_OK)) {
            break;
        }
        count++;
    }

    if (count == 0) {
        /* No profile files on the media (e.g. an old volume): keep what is in
         * flash instead of wiping the user's profiles. */
        (void)tip_profiles_reload();
        return -1;
    }

    import_active();
    (void)tip_profiles_save();

    return (int16_t)count;
}
