#include "usbd_desc.h"
#include "usbd_conf.h"
#include "usbd_ctlreq.h"
#include "stm32g4xx_hal.h"
#include <stdio.h>
#include <string.h>

/* VID/PID: ST's development pair, fine for a hobby/open project. Replace with
 * your own PID if you ever ship this. */
#define USBD_VID                    0x0483U
#define USBD_PID                    0x5720U
#define USBD_LANGID_STRING          0x0409U

#define USBD_MANUFACTURER_STRING    "AxxSolder"
#define USBD_PRODUCT_STRING         "AxxSolder USB Storage"
#define USBD_CONFIGURATION_STRING   "AxxSolder Config"
#define USBD_INTERFACE_STRING       "Mass Storage"

#define USBD_SERIAL_STRING_LEN      25U   /* 24 hex digits + NUL */

static uint8_t USBD_StrDesc[USBD_MAX_STR_DESC_SIZ];
static char    USBD_SerialStr[USBD_SERIAL_STRING_LEN];
static uint8_t USBD_SerialReady;

static uint8_t USBD_DeviceDesc[USB_LEN_DEV_DESC] = {
    USB_LEN_DEV_DESC,               /* bLength */
    USB_DESC_TYPE_DEVICE,           /* bDescriptorType */
    0x00U, 0x02U,                   /* bcdUSB = 2.00 */
    0x00U,                          /* bDeviceClass (defined by the interface) */
    0x00U,                          /* bDeviceSubClass */
    0x00U,                          /* bDeviceProtocol */
    USB_MAX_EP0_SIZE,               /* bMaxPacketSize0 */
    LOBYTE(USBD_VID), HIBYTE(USBD_VID),
    LOBYTE(USBD_PID), HIBYTE(USBD_PID),
    0x00U, 0x02U,                   /* bcdDevice = 2.00 */
    USBD_IDX_MFC_STR,
    USBD_IDX_PRODUCT_STR,
    USBD_IDX_SERIAL_STR,
    USBD_MAX_NUM_CONFIGURATION      /* bNumConfigurations */
};

/* Serial number from the 96-bit factory UID, so several units can be plugged
 * into the same host without a conflict. */
static void USBD_BuildSerial(void)
{
    if (USBD_SerialReady) {
        return;
    }

    (void)snprintf(USBD_SerialStr, sizeof(USBD_SerialStr), "%08lX%08lX%08lX",
                   (unsigned long)HAL_GetUIDw0(),
                   (unsigned long)HAL_GetUIDw1(),
                   (unsigned long)HAL_GetUIDw2());
    USBD_SerialReady = 1;
}

static uint8_t *USBD_FS_DeviceDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    *length = (uint16_t)sizeof(USBD_DeviceDesc);
    return USBD_DeviceDesc;
}

static uint8_t *USBD_FS_LangIDStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;

    *length = 4U;
    USBD_StrDesc[0] = 4U;
    USBD_StrDesc[1] = USB_DESC_TYPE_STRING;
    USBD_StrDesc[2] = LOBYTE(USBD_LANGID_STRING);
    USBD_StrDesc[3] = HIBYTE(USBD_LANGID_STRING);
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_ManufacturerStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_MANUFACTURER_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_ProductStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_PRODUCT_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_SerialStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_BuildSerial();
    USBD_GetString((uint8_t *)USBD_SerialStr, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_ConfigStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_CONFIGURATION_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

static uint8_t *USBD_FS_InterfaceStrDescriptor(USBD_SpeedTypeDef speed, uint16_t *length)
{
    (void)speed;
    USBD_GetString((uint8_t *)USBD_INTERFACE_STRING, USBD_StrDesc, length);
    return USBD_StrDesc;
}

USBD_DescriptorsTypeDef FS_Desc = {
    USBD_FS_DeviceDescriptor,
    USBD_FS_LangIDStrDescriptor,
    USBD_FS_ManufacturerStrDescriptor,
    USBD_FS_ProductStrDescriptor,
    USBD_FS_SerialStrDescriptor,
    USBD_FS_ConfigStrDescriptor,
    USBD_FS_InterfaceStrDescriptor,
};
