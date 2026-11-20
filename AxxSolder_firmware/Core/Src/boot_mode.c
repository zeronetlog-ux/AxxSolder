#include "boot_mode.h"
#include "main.h"
#include "stm32g4xx_hal.h"

/* SW_2 must be held this long for the mode to be selected. Long enough that a
 * short press while plugging the station in cannot trigger it. */
#define BOOT_USB_STORAGE_HOLD_MS   300u
#define BOOT_POLL_MS               10u

bool boot_request_usb_storage(void)
{
    uint32_t held = 0;

    while (held < BOOT_USB_STORAGE_HOLD_MS) {
        if (HAL_GPIO_ReadPin(SW_2_GPIO_Port, SW_2_Pin) != GPIO_PIN_SET) {
            return false;   /* released: normal boot */
        }

        HAL_Delay(BOOT_POLL_MS);
        held += BOOT_POLL_MS;
    }

    return true;
}
