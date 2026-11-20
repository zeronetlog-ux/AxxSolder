#ifndef PROFILE_FILE_H
#define PROFILE_FILE_H

#include <stdint.h>
#include "ff.h"

/* Tip profiles as individual text files on the USB-storage volume:
 *
 *   /P01.CFG … /P24.CFG   one file per profile (name, handle, PID, cal points)
 *   /ACTIVE.CFG           active profile index per handle (-1 = none)
 *
 * Same syntax as AXX.CFG: `key=value` lines, '#' starts a comment, version=1. */

/* Write one file per profile and remove files of profiles that no longer
 * exist. */
FRESULT profile_files_export(void);

/* Rebuild the profile store from the files (one flash write). Returns the
 * number of profiles imported, or -1 when no profile file is present — in that
 * case the store is reloaded from flash and left untouched. */
int16_t profile_files_import(void);

#endif /* PROFILE_FILE_H */
