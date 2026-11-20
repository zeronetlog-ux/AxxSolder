#ifndef USBD_CONF_H
#define USBD_CONF_H

#include "stm32g4xx_hal.h"

/* ---- STM32 USB Device Library configuration ---------------------------- */
/* Only the mass-storage class is used, the device is self-powered by the
 * station supply and never draws more than 100 mA from the host. */

#define USBD_MAX_NUM_INTERFACES      1U
#define USBD_MAX_NUM_CONFIGURATION   1U
#define USBD_MAX_STR_DESC_SIZ        512U
#define USBD_SELF_POWERED            0U
#define USBD_MAX_POWER               100U
#define USBD_DEBUG_LEVEL             0U
#define USBD_LPM_ENABLED             0U
#define USBD_CLASS_BOS_ENABLED       0

/* The class drivers allocate their handle through this single static pool
 * (MSC needs a bit over 600 bytes). No heap is pulled in. */
#define USBD_MALLOC_POOL_SIZE        768U

void *USBD_malloc(uint32_t size);
void  USBD_free(void *ptr);

/* Logging is disabled: every USBD_xxxLog() call compiles to nothing. */
#define USBD_UsrLog(...)   do { } while (0)
#define USBD_ErrLog(...)   do { } while (0)
#define USBD_DbgLog(...)   do { } while (0)

#endif /* USBD_CONF_H */
