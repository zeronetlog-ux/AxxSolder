#ifndef SETTINGS_FILE_H
#define SETTINGS_FILE_H

#include <stdint.h>
#include "ff.h"

/* Text file with user settings, living on the FAT volume that is exposed by
 * the USB-storage mode. Format:
 *
 *   # AxxSolder settings
 *   version=1
 *   startup_temperature=330.00
 *   standby_temp=150.00
 *   ...
 *
 * One "key=value" pair per line, '#' starts a comment, unknown keys are
 * ignored (forward compatibility), keys absent from the file keep their
 * current value. Values are clamped to the ranges used by the settings menu.
 */

#define SETTINGS_FILE_PATH     "/AXX.CFG"
#define SETTINGS_FILE_VERSION  1

/* Write the live flash_values to /AXX.CFG (creates or truncates the file). */
FRESULT settings_file_export(void);

/* Parse /AXX.CFG, apply recognised keys to flash_values and persist them with
 * settings_save(). Returns the number of applied keys, or -1 if the file is
 * missing/unreadable or written by a newer format version. */
int16_t settings_file_import(void);

#endif /* SETTINGS_FILE_H */
