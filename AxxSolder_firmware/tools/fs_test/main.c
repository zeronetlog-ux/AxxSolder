/* Host-side test for the USB-storage data path:
 *
 *   storage_flash_write_page() <-> fs_disk.c page cache <-> FatFS <-> AXX.CFG
 *
 * The MCU flash is replaced by a 20 KB RAM buffer, so the formatting, mount,
 * export and import code can be exercised without hardware.
 *
 * Build/run: tools/fs_test/run.sh
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "host_flash.h"
#include "ff.h"
#include "fs_disk.h"
#include "storage.h"
#include "settings.h"
#include "settings_file.h"

uint8_t host_flash_region[HOST_FLASH_SIZE];

/* ---- Flash replacement ------------------------------------------------ */

bool storage_flash_write_page(uintptr_t page_addr, const uint8_t *page)
{
    uintptr_t offset = page_addr - (uintptr_t)host_flash_region;

    if (offset + FS_DISK_PAGE_SIZE > HOST_FLASH_SIZE) {
        printf("  !! write outside the FS region (offset %u)\n", (unsigned)offset);
        return false;
    }

    memcpy(&host_flash_region[offset], page, FS_DISK_PAGE_SIZE);
    return true;
}

/* ---- Settings stubs --------------------------------------------------- */

Flash_values flash_values;
static int   saves;

void settings_save(void)
{
    saves++;
}

/* ---- Test helpers ----------------------------------------------------- */

static int failures;

static void check(int condition, const char *what)
{
    if (condition) {
        printf("  ok   %s\n", what);
    } else {
        printf("  FAIL %s\n", what);
        failures++;
    }
}

static FRESULT write_file(const char *text)
{
    FIL     file;
    UINT    written;
    FRESULT res = f_open(&file, SETTINGS_FILE_PATH, FA_CREATE_ALWAYS | FA_WRITE);

    if (res != FR_OK) {
        return res;
    }

    res = f_write(&file, text, (UINT)strlen(text), &written);
    (void)f_close(&file);
    return res;
}

static void print_file(const char *dump_path)
{
    FIL  file;
    char buf[128];
    UINT got;
    FILE *dump = dump_path ? fopen(dump_path, "wb") : NULL;

    if (f_open(&file, SETTINGS_FILE_PATH, FA_READ) != FR_OK) {
        printf("  !! cannot open %s\n", SETTINGS_FILE_PATH);
        if (dump) fclose(dump);
        return;
    }

    printf("  --- %s ---\n", SETTINGS_FILE_PATH);
    while ((f_read(&file, buf, sizeof(buf) - 1, &got) == FR_OK) && (got > 0)) {
        buf[got] = '\0';
        if (dump) fwrite(buf, 1, got, dump);
        printf("  %s", buf);
    }
    printf("  --- end ---\n");
    (void)f_close(&file);
    if (dump) fclose(dump);
}

int main(void)
{
    static FATFS fs;
    FRESULT      res;
    LBA_t        sectors = 0;

    printf("AxxSolder USB-storage host test\n");

    /* Erased flash */
    memset(host_flash_region, 0xFF, sizeof(host_flash_region));
    (void)disk_initialize(0);

    printf("\n1) volume is empty / unformatted\n");
    res = f_mount(&fs, "", 1);
    check(res == FR_NO_FILESYSTEM, "mount before format returns FR_NO_FILESYSTEM");
    (void)disk_ioctl(0, GET_SECTOR_COUNT, &sectors);
    check(sectors == 40, "GET_SECTOR_COUNT == 40");

    printf("\n2) formatting\n");
    res = fs_disk_format();
    if (res != FR_OK) {
        printf("  fs_disk_format() -> FRESULT %d\n", (int)res);
    }
    check(res == FR_OK, "fs_disk_format() == FR_OK");
    {
        uint8_t vbr[FS_DISK_SECTOR_SIZE];
        (void)fs_disk_read(vbr, 0, 1);
        printf("  VBR: jmp=%02X%02X%02X oem=%.8s bps=%u spc=%u rsv=%u nfat=%u root=%u tot=%u"
               " media=%02X fat=%u sig=%02X%02X\n",
               vbr[0], vbr[1], vbr[2], &vbr[3],
               (unsigned)(vbr[11] | (vbr[12] << 8)),
               (unsigned)vbr[13], (unsigned)vbr[14], (unsigned)vbr[15],
               (unsigned)(vbr[16] | (vbr[17] << 8)),
               (unsigned)(vbr[18] | (vbr[19] << 8)),
               (unsigned)vbr[21],
               (unsigned)(vbr[22] | (vbr[23] << 8)),
               (unsigned)vbr[510], (unsigned)vbr[511]);
        printf("  flash: jmp=%02X%02X%02X oem=%.8s tot=%u sig=%02X%02X\n",
               host_flash_region[0], host_flash_region[1], host_flash_region[2],
               &host_flash_region[3],
               (unsigned)(host_flash_region[18] | (host_flash_region[19] << 8)),
               (unsigned)host_flash_region[510], (unsigned)host_flash_region[511]);
    }
    res = f_mount(&fs, "", 1);
    if (res != FR_OK) {
        printf("  mount after format -> FRESULT %d\n", (int)res);
    }
    check(res == FR_OK, "mount after format == FR_OK");

    printf("\n3) export current settings\n");
    flash_values.startup_temperature = 300.0f;
    flash_values.standby_temp        = 150.0f;
    flash_values.preset_temp_2       = 444.0f;
    flash_values.standby_time        = 7.0f;
    flash_values.buzzer_enabled      = 0.0f;
    flash_values.temperature_offset  = -2.5f;

    res = settings_file_export();
    check(res == FR_OK, "settings_file_export() == FR_OK");
    print_file("/tmp/axx_cfg_export.bin");

    printf("\n4) host edits the file (FatFS write path)\n");
    res = write_file(
        "# AxxSolder settings\n"
        "version=1\n"
        "startup_temperature=360.5\n"
        "preset_temp_2=99\n"
        "power_limit_T245=500\n"      /* > MAX_POWER -> clamped */
        "screen_rotation=7\n"          /* > 3 -> clamped */
        "buzzer_enabled=0\n"
        "unknown_key=123\n"            /* ignored */
        "standby_time=abc\n");         /* unparsable -> ignored */
    check(res == FR_OK, "host write == FR_OK");

    printf("\n5) import\n");
    saves = 0;
    int16_t applied = settings_file_import();
    printf("  applied=%d saves=%d\n", (int)applied, saves);
    check(applied == 5, "5 keys recognised (comment, unknown and invalid ignored)");
    check(saves == 1, "settings_save() called once");
    check(flash_values.startup_temperature > 360.4f &&
          flash_values.startup_temperature < 360.6f, "fractional value parsed (360.5)");
    check(flash_values.preset_temp_2 == 99.0f, "preset_temp_2 == 99");
    check(flash_values.power_limit_T245 == 150.0f, "power limit clamped to 150");
    check(flash_values.screen_rotation == 3.0f, "screen_rotation clamped to 3");
    check(flash_values.standby_time == 7.0f, "unparsable value left untouched");
    check(flash_values.temperature_offset == -2.5f, "untouched negative value kept");

    printf("\n6) file from a 'newer' firmware is rejected\n");
    res = write_file("version=2\nstartup_temperature=111\n");
    check(res == FR_OK, "host write == FR_OK");
    saves = 0;
    check(settings_file_import() == -1, "import returns -1");
    check(flash_values.startup_temperature > 360.4f, "settings unchanged");
    check(saves == 0, "settings_save() not called");

    printf("\n7) persistence: remount and read the file again\n");
    (void)f_mount(NULL, "", 0);
    res = f_mount(&fs, "", 1);
    check(res == FR_OK, "remount == FR_OK");
    print_file("/tmp/axx_cfg_after.bin");

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED",
           failures, (failures == 1) ? "" : "s");
    return failures ? 1 : 0;
}
