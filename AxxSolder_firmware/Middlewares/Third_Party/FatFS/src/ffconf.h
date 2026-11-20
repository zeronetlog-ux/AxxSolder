/*----------------------------------------------------------------------------/
/  FatFs - Configuration file for AxxSolder USB-storage mode                   /
/                                                                              /
/  Sized for the STM32G431 (32 KB RAM) and an 8 KB internal-flash volume:      /
/   - tiny mode keeps the FAT window out of every FATFS object                 /
/   - no LFN (8.3 names only: AXX.CFG), no unicode, no RTC                     /
/   - FF_USE_MKFS is needed once to format the volume on first entry           /
/----------------------------------------------------------------------------*/

#ifndef FFCONF_DEF
#define FFCONF_DEF  80386   /* Revision ID - must match FF_DEFINED in ff.h */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY  0
/* This option switches read-only configuration. (0:Read/Write or 1:Read-only)
/  Read-only configuration removes basic writing API functions. */

#define FF_FS_MINIMIZE  0
/* 0: All basic functions are enabled. */

#define FF_USE_FIND     0
#define FF_USE_MKFS     0
/* f_mkfs() is not linked: it refuses volumes below 32 KB, so fs_disk.c writes
 * the FAT12 boot sector / FAT / root directory itself. */

#define FF_USE_FASTSEEK 0
#define FF_USE_EXPAND   0
#define FF_USE_CHMOD    0
#define FF_USE_LABEL    0
#define FF_USE_FORWARD  0

#define FF_USE_STRFUNC  0
#define FF_PRINT_LLI    0
#define FF_PRINT_FLOAT  0
#define FF_STRF_ENCODE  0
/* f_gets/f_puts are not used - saves code size. */

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE    437
/* OEM code page. 437 = US. Only 8.3 names are used (FF_USE_LFN = 0). */

/* FatFS "implementation limits" from ff.c, overridden here: a 40-sector
 * (20 KB) FAT12 volume with 37 clusters is valid per the FAT specification but
 * below FatFS's heuristics (64 sectors / 32 clusters), which would otherwise
 * require a >= 32 KB volume that does not fit next to the firmware. The volume
 * is created by fs_disk_format() and only ever touched by this firmware. */
#define MIN_VOLUME      40      /* sectors */
#define MIN_FAT12       32      /* clusters (volume has 37) */

#define FF_USE_LFN      0
#define FF_MAX_LFN      255
#define FF_LFN_UNICODE  0
#define FF_LFN_BUF      255
#define FF_SFN_BUF      12

#define FF_FS_RPATH     0
#define FF_PATH_DEPTH   10

#define FF_VOLUMES      1
#define FF_STR_VOLUME_ID   0
#define FF_VOLUME_STRS  "RAM","NAND","CF","SD","SD2","USB","USB2","USB3"

#define FF_MULTI_PARTITION  0

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_MIN_SS       512
#define FF_MAX_SS       512
/* Single sector size - no runtime buffer size logic. */

#define FF_LBA64        0
#define FF_MIN_GPT      0x10000000
#define FF_USE_TRIM     0

#define FF_FS_TINY      1
/* Tiny mode: the sector window lives in the file object instead of every
/  FATFS object - saves 512 B of BSS on this RAM-tight target. */

#define FF_FS_EXFAT     0

#define FF_FS_NORTC     1
#define FF_NORTC_MON    1
#define FF_NORTC_MDAY   1
#define FF_NORTC_YEAR   2025
/* No RTC on the board: timestamps come from these fixed values. */

#define FF_FS_CRTIME    0
#define FF_FS_NOFSINFO  0
#define FF_FS_LOCK      0

#define FF_FS_REENTRANT 0
#define FF_FS_TIMEOUT   1000

#endif /* FFCONF_DEF */
