#include "settings.h"
#include "storage.h"
#include "buzzer.h"
#include "stm32g4xx_hal.h"
#include <string.h>

/* Struct to hold flash values */
Flash_values flash_values;
uint8_t      settings_menu_active = 0;

Flash_values default_flash_values = {
    .startup_temperature           = 330,
    .temperature_offset            = 0,
    .standby_temp                  = 150,
    .standby_time                  = 10,
    .emergency_time                = 30,
    .buzzer_enabled                = 1,
    .preset_temp_1                 = 330,
    .preset_temp_2                 = 430,
    .GPIO4_ON_at_run               = 0,
    .screen_rotation               = 0,
    .momentary_stand               = 0,
    .current_measurement           = 1,
    .startup_beep                  = 1,
    .deg_celsius                   = 1,
    .temp_cal_100                  = 100,
    .temp_cal_200                  = 200,
    .temp_cal_300                  = 300,
    .temp_cal_350                  = 350,
    .temp_cal_400                  = 400,
    .temp_cal_450                  = 450,
    .serial_debug_print            = 0,
    .displayed_temp_filter         = 5,
    .startup_temp_is_previous_temp = 0,
    .three_button_mode             = 0,
    .beep_at_set_temp              = 1,
    .beep_tone                     = 0,
    .power_unit                    = 0,
    .detect_nt115                  = 1,
    .power_limit_T245              = 0,
    .power_limit_T210              = 0,
    .power_limit_NT115             = 0,
    .power_limit_No_name           = 0,
    .display_graph                 = 0,
    .delta_t_detection             = 1,
    .standby_delay                 = 0,
    .show_profile_on_tip_change    = 0,
    .change_enc_dir                = 0,
    .encoder_step_idx              = 1,  /* index 1 -> step 2 */
    .heat_at_startup               = 0,
    .pd_profile                    = -1, /* USB-PD: automatic (strongest) */
    .pd_auto_max                   = 0,  /* USB-PD: show the mode chooser   */
    .graph_timebase                = 1,  /* 1 s/div */
    .preset_temp_3                 = 280,
    .preset_temp_4                 = 380
};

/* ------------------------------------------------------------------ */
/* Init / save                                                        */
/* ------------------------------------------------------------------ */

void settings_init(void){
	// Check if user data in flash is valid, if not - write default parameters
	if(!STORAGE_SETTINGS_DRIVER->verify(SETTINGS_PAGE, sizeof(Flash_values))){
		/* Records from older firmware are shorter by the fields appended
		 * since: no preset 3/4 (2 floats) or no graph time base either
		 * (3 floats). Keep the user's values and append defaults. */
		const uint16_t prev_no_presets = (uint16_t)(sizeof(Flash_values) - 2 * sizeof(float));
		const uint16_t prev_no_graph   = (uint16_t)(sizeof(Flash_values) - 3 * sizeof(float));
		uint16_t       old_size        = 0;

		if(STORAGE_SETTINGS_DRIVER->verify(SETTINGS_PAGE, prev_no_presets)){
			old_size = prev_no_presets;
		}
		else if(STORAGE_SETTINGS_DRIVER->verify(SETTINGS_PAGE, prev_no_graph)){
			old_size = prev_no_graph;
		}

		if(old_size != 0){
			memcpy(&flash_values, &default_flash_values, sizeof(Flash_values));
			if(STORAGE_SETTINGS_DRIVER->read(SETTINGS_PAGE, &flash_values, old_size) == STORE_OK){
				settings_save();          /* upgrade the stored record */
			}
			return;
		}

		STORAGE_SETTINGS_DRIVER->write(SETTINGS_PAGE, &default_flash_values, sizeof(Flash_values));
		beep(1); //Beep once to indicate default parameters written to flash
		HAL_Delay(100);
	}

	/* Read flash data */
	STORAGE_SETTINGS_DRIVER->read(SETTINGS_PAGE, &flash_values, sizeof(Flash_values));
}

void settings_save(void) {
    STORAGE_SETTINGS_DRIVER->write(SETTINGS_PAGE, &flash_values, sizeof(Flash_values));
}
