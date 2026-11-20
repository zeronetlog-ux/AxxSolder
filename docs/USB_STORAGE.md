# USB storage mode (settings file over USB)

Besides editing the settings in the on-screen menu, AxxSolder can publish its
settings as a small text file on a read/write disk that appears on a PC. The
feature is meant for bulk editing, backup/restore and for sharing settings
between stations.

---

## Entering and leaving the mode

1. **Hold the `preset 1` button (`SW_2`) while the station is powered up.**
   The button has to be held for ~0.3 s from the moment power is applied.
2. The display shows `USB STORAGE MODE` and the station appears on the host as
   a removable drive named `AXXSOLDER` with a single file, `AXX.CFG`.
3. Edit the file with any text editor, save it and eject/unmount the volume.
4. **Ejecting the volume reboots the station into the normal firmware** (the
   pending edits are applied first, the display shows `Rebooting...`). Note that
   a plain *unmount* (`diskutil unmount`, "Unmount" in Disk Utility) does not
   send a SCSI eject, so it only applies the edits - use *Eject*
   (Finder's ⏏ button / `diskutil eject`) to leave the mode. Removing the power
   also works and is still the fallback.

The firmware watches the SCSI medium state of the class driver: an eject
(`START STOP UNIT` with LoEj) or an unmount (`ALLOW MEDIUM REMOVAL` with
prevent=0) sets the exit request, then the main loop waits until the host has
been quiet for 400 ms, imports `AXX.CFG`, flushes the flash page and calls
`NVIC_SystemReset()`. The station then starts normally, without the USB mode.

While in this mode the heater is disabled for the whole session: TIM1 is
initialised with zero duty exactly like during a normal boot, but the control
loop (PID, state machine, sensors, current measurement) is never started, so
the cartridge cannot heat up.

---

## How the disk is built

The station has no SD card slot (the SPI2 MISO line is used as the LCD chip
select), so the FAT volume lives in the STM32G431 internal flash.

| Region | Address | Size | Purpose |
|---|---|---|---|
| `FLASH` | `0x08000000` | 106 KB | Firmware |
| `FS` | `0x0801A800` | 18 KB | FAT12 volume (36 sectors × 512 B) |
| `PROFILES` | `0x0801F000` | 2 KB | Tip profiles |
| `SETTINGS` | `0x0801F800` | 2 KB | User settings |

The volume uses 512-byte clusters, a single FAT and a 64-entry root directory,
which leaves 30 clusters for data. It is formatted on the first entry into the
mode (`fs_disk_format()` in `Core/Src/fs_disk.c`) — the boot sector, FAT and
root directory are written directly, because `f_mkfs()` refuses volumes smaller
than 32 KB (`MIN_VOLUME`), which does not fit next to the firmware.

Two FatFS "implementation limits" are therefore overridden in
`Middlewares/Third_Party/FatFS/src/ffconf.h` (`MIN_VOLUME 36`, `MIN_FAT12 24`);
a 36-sector FAT12 volume is valid per the FAT specification, and the volume is
only ever touched by this firmware or by a host writing the settings file.

### Flash wear and interrupts

Flash can only be erased page-wise (2 KB), so `fs_disk.c` keeps one 2 KB page
in a cache and writes it back with read-modify-write when the cache moves to
another page or when the host sends SYNCHRONIZE CACHE. A page erase/program
takes tens of milliseconds with interrupts disabled — harmless here because
nothing else runs in this mode.

Every entry into the mode rewrites `AXX.CFG` from the current settings, and the
file is only written back to the settings page when a value actually changed,
which keeps the number of page erases low.

---

## `AXX.CFG` format
```
# AxxSolder settings
# key=value, one per line, '#' starts a comment
# Unplug power from the soldering iron to leave USB-storage mode.
version=1
startup_temperature=330.00
temperature_offset=0.00
standby_temp=150.00
...
heat_at_startup=0.00
pd_profile=-1.00
```

- One `key=value` pair per line, CR/LF or LF, `#` or `;` starts a comment.
- `version` is the format version; a file with a newer version is ignored.
- Unknown keys are ignored (a file from a newer firmware is still usable).
- Values are clamped to the same ranges the settings menu uses
  (`Core/Src/settings_file.c`), so a typo cannot put the controller into a
  state the menu could not reach.
- `1`/`0` are used for the yes/no settings, temperatures are plain numbers
  with two decimals.
- Changes are applied ~1 s after the host stops writing (the display shows
  `Applied N settings`), so close/save the file and wait a moment before
  pulling the plug. The values are then written to the settings page, i.e.
  they survive the next normal boot.

Key names are the field names of `Flash_values` (`Core/Inc/settings.h`), which
matches the settings menu order:

`startup_temperature`, `temperature_offset`, `standby_temp`, `standby_time`,
`emergency_time`, `buzzer_enabled`, `preset_temp_1`, `preset_temp_2`,
`GPIO4_ON_at_run`, `screen_rotation`, `momentary_stand`, `current_measurement`,
`startup_beep`, `deg_celsius`, `temp_cal_100` … `temp_cal_450`,
`serial_debug_print`, `displayed_temp_filter`, `startup_temp_is_previous_temp`,
`three_button_mode`, `beep_at_set_temp`, `beep_tone`, `power_unit`,
`detect_nt115`, `power_limit_T245`, `power_limit_T210`, `power_limit_NT115`,
`power_limit_No_name`, `display_graph`, `delta_t_detection`, `standby_delay`,
`show_profile_on_tip_change`, `change_enc_dir`, `encoder_step_idx`,
`heat_at_startup`, `pd_profile`.

`pd_profile` is the USB-PD voltage the station prefers at start-up: `-1` picks
the most powerful mode the source offers, any other value (e.g. `15`) prefers
that voltage and falls back to the most powerful mode when the source has no PDO
there. The list shown at boot is the source's own capability list — the voltage
and current it advertises for each of its PDOs.

---

## Tip profiles: `P01.CFG` … `Pnn.CFG`

Every tip profile is its own small file, `/P01.CFG`, `/P02.CFG`, … in the same
order as in the profiles menu. The files are contiguous: the import stops at the
first file that is missing or contains no keys, so the firmware writes a
header-only file right after the last profile instead of deleting stale files.

```
# AxxSolder tip profile
version=1
name=Default
handle=T245
default=1
kp=8.00
ki=2.00
kd=0.50
max_i=300.00
power_limit=0.00
temp_cal_100=100.00
temp_cal_200=200.00
temp_cal_300=300.00
temp_cal_350=350.00
temp_cal_400=400.00
temp_cal_450=450.00
```

- `handle` is `NT115`, `T210`, `T245` or `No_name`.
- `name` is the display name shown in the menu (max 23 characters).
- `power_limit` is in watts, `0` = no profile limit.
- Values are clamped on import (kp/ki/kd 0…100, max_i 0…1000, power_limit
  0…MAX_POWER, calibration points 0…600).

`/ACTIVE.CFG` stores which profile is active per handle type:

```
# active profile index per handle, -1 = none
nt115=0
t210=-1
t245=1
no_name=-1
```

`-1` (or `255`) means "no profile active"; indexes are validated against the
number of imported profiles. All profiles are written to flash with a single
`tip_profiles_save()` after a successful import.

The volume is 18 KB (36 sectors, 30 clusters of 512 B, 64 root entries), which
fits 24 profiles plus `AXX.CFG` and the metadata files a host may create.

---

## Firmware structure

| File | Purpose |
|---|---|
| `Core/Src/boot_mode.c` | Detects `SW_2` held at power-up |
| `Core/Src/usb_mode.c` | Mode orchestration, screen, HSI48 + CRS, mount/format, import loop |
| `Core/Src/usbd_conf.c` | PCD/USBD low-level glue (static class allocation, no heap) |
| `Core/Src/usbd_desc.c` | Device and string descriptors (`AxxSolder USB Storage`) |
| `Core/Src/usb_msc_if.c` | MSC storage callbacks → `fs_disk_*` |
| `Core/Src/fs_disk.c` | FatFS disk I/O over internal flash, page cache, FAT12 formatter |
| `Core/Src/settings_file.c` | `AXX.CFG` export/import, key table and clamping |
| `Middlewares/ST/STM32_USB_Device_Library` | ST USB device core + MSC class (vendored) |
| `Middlewares/Third_Party/FatFS` | FatFS R0.14 (vendored) |

Clock: the board has no crystal, so USB runs from HSI48
(`RCC_CCIPR.CLK48SEL` reset value); the PLLQ output (85 MHz) cannot be used.
CRS trimming against the host SOF is currently **not** enabled — a wrong
`RELOAD`/`FELIM` would trim HSI48 away from 48 MHz, and the free-running HSI48
is accurate enough for full speed. Enable it only with a validated
configuration.

The USB interrupt and the D+ pull-up are only enabled in `USBD_LL_Start()`, so
a normal boot is invisible on the USB port. The volume is mounted and
`AXX.CFG` written **before** the pull-up is enabled: if the host probes the
media while it is still unformatted it marks the disk unreadable and ejects it.

`USE_HAL_PCD_REGISTER_CALLBACKS` is 0, so the USB device library receives bus
events only through the HAL callbacks `HAL_PCD_SetupStageCallback()`,
`DataIn`/`DataOut`, `Reset`, `SOF`, ... These are implemented in
`Core/Src/usbd_conf.c`. Without them the device still attaches (SOF interrupts
are visible, `IRQ` counter in the diagnostics line grows) but SETUP packets are
never processed and the host never enumerates it — the symptom is "device is
powered and attached, but no device and no disk on the PC".

FatFS build options (`ffconf.h`): read/write, `FF_FS_TINY 1`, 512-byte sectors,
no LFN/Unicode/RTC, no `f_mkfs`.

---

## Testing without hardware

`tools/fs_test/run.sh` builds the disk layer, FatFS and the settings-file code
for the host, replacing the flash with a 20 KB RAM buffer, and verifies:

- the volume is detected as unformatted, then formatted and mounted;
- export writes all settings;
- a host edit is parsed, clamped, and persisted exactly once;
- a file with a newer `version` is rejected;
- the volume survives unmount/remount.

Run it after touching `fs_disk.c`, `settings_file.c` or `ffconf.h`.
