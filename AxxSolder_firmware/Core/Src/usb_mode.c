#include "usb_mode.h"
#include "usb_msc_if.h"
#include "usb_device.h"
#include "usbd_core.h"
#include "usbd_desc.h"
#include "usbd_msc.h"
#include "fs_disk.h"
#include "ff.h"
#include "settings.h"
#include "settings_file.h"
#include "profile_file.h"
#include "tip_profile.h"       /* tip_profiles_init() + STORAGE_PROFILES_DRIVER */
#include "lcd.h"
#include "gui.h"
#include "app_colors.h"
#include "buzzer.h"
#include "main.h"
#include "stm32g4xx_hal.h"
#include <stdio.h>
#include <string.h>

/* How long the USB bus must be quiet before /AXX.CFG is parsed. */
#define CFG_IDLE_MS        1000u

static FATFS fs;

static volatile bool     cfg_dirty;
static volatile uint32_t cfg_dirty_tick;

/* ------------------------------------------------------------------ */
/* Settings-file synchronisation                                      */
/* ------------------------------------------------------------------ */

void usb_mode_mark_cfg_dirty(void)
{
    cfg_dirty      = true;
    cfg_dirty_tick = HAL_GetTick();
}

/* ------------------------------------------------------------------ */
/* Screen                                                             */
/* ------------------------------------------------------------------ */

static void screen_status(const char *text, uint16_t color)
{
    UG_FillFrame(6, 118, 234, 140, C_Background);
    LCD_PutStr(8, 120, (char *)text, FONT_arial_12X15, color, C_Background);
}

static void screen_draw_static(void)
{
    LCD_PutStr(8, 10, (char *)"USB STORAGE MODE", FONT_arial_17X18, C_Header, C_Background);
    LCD_PutStr(8, 40, (char *)"Heater disabled", FONT_arial_12X15, C_Text, C_Background);
    LCD_PutStr(8, 58, (char *)"AXX.CFG: settings", FONT_arial_12X15, C_Text, C_Background);
    LCD_PutStr(8, 74, (char *)"P01..Pnn: profiles", FONT_arial_12X15, C_Text, C_Background);
    LCD_PutStr(8, 160, (char *)"Edit, save, eject", FONT_arial_12X15, C_Text, C_Background);
    LCD_PutStr(8, 236, (char *)"Eject disk to reboot", FONT_arial_12X15, C_Header, C_Background);
    LCD_PutStr(8, 252, (char *)"or unplug power", FONT_arial_12X15, C_Warning, C_Background);
}

static void screen_fatal(const char *reason)
{
    UG_FillScreen(C_Background);
    LCD_PutStr(8, 10, (char *)"USB STORAGE ERROR", FONT_arial_17X18, C_Warning, C_Background);
    LCD_PutStr(8, 40, (char *)reason, FONT_arial_12X15, C_Text, C_Background);
    LCD_PutStr(8, 200, (char *)"UNPLUG POWER TO EXIT", FONT_arial_12X15, C_Warning, C_Background);
}

/* ------------------------------------------------------------------ */
/* Clocks: HSI48 + CRS                                                */
/* ------------------------------------------------------------------ */

/* There is no crystal on the board, so the USB 48 MHz clock comes from HSI48
 * (RCC_CCIPR.CLK48SEL = HSI48 by reset value; PLLQ would be 85 MHz).
 *
 * CRS trimming against the host SOF is intentionally NOT enabled: a wrong
 * RELOAD/FELIM setting would trim HSI48 away from 48 MHz and corrupt
 * enumeration, while the free-running HSI48 is accurate enough for USB full
 * speed. Enable it only after the link has been validated on hardware.
 * Returns false if HSI48 does not come up (reported on the screen). */
static bool usb_clock_init(void)
{
    __HAL_RCC_HSI48_ENABLE();

    for (uint32_t i = 0; i < 100u; i++) {
        if (__HAL_RCC_GET_FLAG(RCC_FLAG_HSI48RDY) != 0u) {
            return true;
        }
        HAL_Delay(1);                       /* bounded wait, no hang */
    }

    return false;
}

/* ------------------------------------------------------------------ */
/* FAT volume                                                         */
/* ------------------------------------------------------------------ */

static FRESULT volume_mount(void)
{
    FRESULT res = f_mount(&fs, "", 1);

    if (res == FR_NO_FILESYSTEM) {
        /* First entry (or the volume was damaged): format it. */
        res = fs_disk_format();
        if (res == FR_OK) {
            res = f_mount(&fs, "", 1);
        }
    }

    return res;
}

/* ------------------------------------------------------------------ */
/* Mode entry                                                         */
/* ------------------------------------------------------------------ */

/* If the host ejects the volume (Finder eject / "safely remove"), wait for the
 * last transfer to finish, apply the edits and reboot into the normal firmware
 * instead of forcing the user to pull the power. */
#define USB_EXIT_IO_QUIET_MS   400u

static void leave_to_normal_boot(void)
{
    /* Apply whatever the host wrote before restarting. */
    (void)settings_file_import();
    (void)profile_files_import();
    (void)fs_disk_sync();

    screen_status("Rebooting...", C_Header);
    beep(flash_values.buzzer_enabled);
    HAL_Delay(250);

    NVIC_SystemReset();
}

void usb_storage_mode_run(void)
{
    /* Buttons are not used in this mode. Their EXTI handler would start the
     * (never initialised) debounce timer as soon as SW_2 is released, so the
     * whole button path is detached first. */
    EXTI->IMR1 &= ~(uint32_t)(SW_1_Pin | SW_2_Pin | SW_3_Pin);
    EXTI->PR1   = (uint32_t)(SW_1_Pin | SW_2_Pin | SW_3_Pin);
    HAL_NVIC_DisableIRQ(EXTI9_5_IRQn);

    /* The power stage must stay off for the whole session. */
    app_heater_hardware_off();

    /* Settings are read (and defaults written only if the CRC is invalid). */
    settings_init();

    /* The profile store is loaded in the normal boot path, which this mode
     * skips - but the file export below needs it (and the driver pointer). */
    tip_profiles_init(STORAGE_PROFILES_DRIVER);

    LCD_init();
    LCD_SetRotation((uint8_t)flash_values.screen_rotation);
    UG_FontSetTransparency(1);
    UG_FillScreen(C_Background);
    screen_draw_static();
    screen_status("Waiting for USB host...", C_Text);

    beep(flash_values.buzzer_enabled);

    screen_status("USB clock...", C_Text);
    if (!usb_clock_init()) {
        screen_fatal("No 48MHz clock");
        for (;;) { }
    }

    screen_status("USB init...", C_Text);
    if (USBD_Init(&hUsbDeviceFS, &FS_Desc, 0U) != USBD_OK) {
        screen_fatal("USBD failed");
        for (;;) { }
    }
    if (USBD_RegisterClass(&hUsbDeviceFS, &USBD_MSC) != USBD_OK) {
        screen_fatal("USBD class failed");
        for (;;) { }
    }
    if (USBD_MSC_RegisterStorage(&hUsbDeviceFS, &USBD_MSC_fops) != USBD_OK) {
        screen_fatal("USBD storage failed");
        for (;;) { }
    }

    /* Attach only after the volume is ready: if the host probes the media
     * before it is formatted it marks the disk as unreadable and ejects it. */
    screen_status("Mounting volume...", C_Text);
    FRESULT res = volume_mount();
    if (res != FR_OK) {
        char msg[40];
        (void)snprintf(msg, sizeof(msg), "Mount failed (%d)", (int)res);
        screen_fatal(msg);
        for (;;) { }
    }

    usb_msc_if_set_ready(true);

    /* Publish current settings and profiles: the host always starts from a
     * complete set of files. */
    screen_status("Writing files...", C_Text);
    if ((settings_file_export() != FR_OK) || (profile_files_export() != FR_OK)) {
        screen_status("Write failed", C_Warning);
    }

    /* Only now enable the D+ pull-up and let the host enumerate. */
    screen_status("Attaching...", C_Text);
    if (USBD_Start(&hUsbDeviceFS) != USBD_OK) {
        screen_fatal("USBD start failed");
        for (;;) { }
    }

    UG_FillFrame(198, 108, 240, 158, C_Background);
    LCD_PutStr(200, 118, (char *)"OK", FONT_arial_17X18, C_Header, C_Background);
    screen_status("Disk ready", C_Header);
    beep_double(flash_values.buzzer_enabled);

    uint8_t  prev_medium = SCSI_MEDIUM_UNLOCKED;
    bool     exit_pending = false;

    for (;;) {
        /* Host ejected the medium (Finder eject / "safely remove" sends
         * START STOP UNIT with LoEj) or released it (unmount sends ALLOW
         * MEDIUM REMOVAL with prevent=0): reboot into the normal firmware. */
        uint8_t medium = usb_msc_medium_state();

        if ((medium == SCSI_MEDIUM_EJECTED) ||
            ((prev_medium == SCSI_MEDIUM_LOCKED) && (medium == SCSI_MEDIUM_UNLOCKED))) {
            exit_pending = true;
        }
        prev_medium = medium;

        if (exit_pending &&
            ((HAL_GetTick() - usb_msc_last_io_tick()) > USB_EXIT_IO_QUIET_MS)) {
            leave_to_normal_boot();     /* does not return */
        }

        if (cfg_dirty && ((HAL_GetTick() - cfg_dirty_tick) > CFG_IDLE_MS)) {
            cfg_dirty = false;

            int16_t applied  = settings_file_import();
            int16_t profiles = profile_files_import();
            char    msg[40];

            if (applied < 0) {
                (void)snprintf(msg, sizeof(msg), "Bad cfg version");
                screen_status(msg, C_Warning);
            } else if ((applied == 0) && (profiles <= 0)) {
                (void)snprintf(msg, sizeof(msg), "No changes");
                screen_status(msg, C_Text);
            } else {
                (void)snprintf(msg, sizeof(msg), "Applied %d, %d profiles",
                               (int)applied, (int)((profiles > 0) ? profiles : 0));
                screen_status(msg, C_Header);
                beep(flash_values.buzzer_enabled);
            }
        }
    }
}
