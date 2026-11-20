#include "usbd_conf.h"
#include "usbd_core.h"
#include "usbd_msc.h"
#include "usb_device.h"
#include "main.h"          /* Error_Handler() */

/* The MSC class handle is allocated from the static pool in usbd_conf.h. */
_Static_assert(sizeof(USBD_MSC_BOT_HandleTypeDef) <= USBD_MALLOC_POOL_SIZE,
               "USBD_MALLOC_POOL_SIZE is too small for the MSC class handle");

/* USB-device handles (declared in usb_device.h) */
PCD_HandleTypeDef  hpcd_USB_FS;
USBD_HandleTypeDef hUsbDeviceFS;

/* Diagnostics: number of USB interrupts taken (host activity indicator). */
volatile uint32_t usb_irq_count;
volatile uint32_t usb_reset_count;
volatile uint32_t usb_ctr_count;
volatile uint32_t usb_err_count;

/* ------------------------------------------------------------------ */
/* Static allocation for the class drivers                            */
/* ------------------------------------------------------------------ */

static uint32_t usbd_malloc_pool[USBD_MALLOC_POOL_SIZE / 4U];
static uint8_t  usbd_malloc_used;

void *USBD_malloc(uint32_t size)
{
    if (usbd_malloc_used || (size > sizeof(usbd_malloc_pool))) {
        return NULL;
    }
    usbd_malloc_used = 1;
    return usbd_malloc_pool;
}

void USBD_free(void *ptr)
{
    (void)ptr;
    usbd_malloc_used = 0;
}

/* ------------------------------------------------------------------ */
/* PCD MSP / interrupt                                                */
/* ------------------------------------------------------------------ */

void HAL_PCD_MspInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB) {
        __HAL_RCC_USB_CLK_ENABLE();
        /* The USB interrupt itself is enabled in USBD_LL_Start(), once the
         * device handle is fully initialised. */
        HAL_NVIC_SetPriority(USB_LP_IRQn, 0, 0);
    }
}

void HAL_PCD_MspDeInit(PCD_HandleTypeDef *pcdHandle)
{
    if (pcdHandle->Instance == USB) {
        HAL_NVIC_DisableIRQ(USB_LP_IRQn);
        __HAL_RCC_USB_CLK_DISABLE();
    }
}

/* Vector from the startup file */
void USB_LP_IRQHandler(void)
{
    uint16_t istr = USB->ISTR;

    usb_irq_count++;
    if ((istr & USB_ISTR_RESET) != 0u) {
        usb_reset_count++;
    }
    if ((istr & USB_ISTR_CTR) != 0u) {
        usb_ctr_count++;
    }
    if ((istr & USB_ISTR_ERR) != 0u) {
        usb_err_count++;
    }

    HAL_PCD_IRQHandler(&hpcd_USB_FS);
}

/* ------------------------------------------------------------------ */
/* HAL PCD bus-event callbacks -> USB device library                   */
/* ------------------------------------------------------------------ */
/* USE_HAL_PCD_REGISTER_CALLBACKS is 0, so the HAL calls these weak
 * functions. They are the glue that feeds bus events into USBD_LL_*():
 * without them SETUP/IN/OUT stages are never processed and the device
 * never enumerates (SOF interrupts would still be visible). */
void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_SetupStage((USBD_HandleTypeDef *)hpcd->pData, (uint8_t *)hpcd->Setup);
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_DataOutStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                               hpcd->OUT_ep[epnum].xfer_buff);
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_DataInStage((USBD_HandleTypeDef *)hpcd->pData, epnum,
                              hpcd->IN_ep[epnum].xfer_buff);
}

void HAL_PCD_SOFCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_SOF((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_SetSpeed((USBD_HandleTypeDef *)hpcd->pData, USBD_SPEED_FULL);
    (void)USBD_LL_Reset((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_Suspend((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ResumeCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_Resume((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ConnectCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_DevConnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_DisconnectCallback(PCD_HandleTypeDef *hpcd)
{
    (void)USBD_LL_DevDisconnected((USBD_HandleTypeDef *)hpcd->pData);
}

void HAL_PCD_ISOOUTIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_IsoOUTIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

void HAL_PCD_ISOINIncompleteCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)USBD_LL_IsoINIncomplete((USBD_HandleTypeDef *)hpcd->pData, epnum);
}

/* ------------------------------------------------------------------ */
/* Low-level (PCD) callbacks                                          */
/* ------------------------------------------------------------------ */

USBD_StatusTypeDef USBD_LL_Init(USBD_HandleTypeDef *pdev)
{
    hpcd_USB_FS.pData = pdev;

    hpcd_USB_FS.Instance                 = USB;
    hpcd_USB_FS.Init.dev_endpoints       = 8U;
    hpcd_USB_FS.Init.speed               = PCD_SPEED_FULL;
    hpcd_USB_FS.Init.phy_itface          = PCD_PHY_EMBEDDED;
    hpcd_USB_FS.Init.Sof_enable          = DISABLE;
    hpcd_USB_FS.Init.low_power_enable    = DISABLE;
    hpcd_USB_FS.Init.lpm_enable          = DISABLE;
    hpcd_USB_FS.Init.battery_charging_enable = DISABLE;
    hpcd_USB_FS.Init.ep0_mps             = 64U;

    if (HAL_PCD_Init(&hpcd_USB_FS) != HAL_OK) {
        return USBD_FAIL;
    }

    /* PMA layout, same as ST's STM32G4 MSC example:
     * EP0 OUT 0x10, EP0 IN 0x50, EP1 IN 0x90, EP1 OUT 0xD0 (64-byte buffers). */
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x00U, PCD_SNG_BUF, 0x10U);
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x80U, PCD_SNG_BUF, 0x50U);
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x81U, PCD_SNG_BUF, 0x90U);
    (void)HAL_PCDEx_PMAConfig(&hpcd_USB_FS, 0x01U, PCD_SNG_BUF, 0xD0U);

    pdev->pData = &hpcd_USB_FS;
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_DeInit(USBD_HandleTypeDef *pdev)
{
    (void)pdev;
    (void)HAL_PCD_DeInit(&hpcd_USB_FS);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Start(USBD_HandleTypeDef *pdev)
{
    (void)pdev;
    HAL_NVIC_EnableIRQ(USB_LP_IRQn);   /* safe now: the handle is initialised */
    (void)HAL_PCD_Start(&hpcd_USB_FS);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Stop(USBD_HandleTypeDef *pdev)
{
    (void)pdev;
    HAL_NVIC_DisableIRQ(USB_LP_IRQn);
    (void)HAL_PCD_Stop(&hpcd_USB_FS);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                  uint8_t ep_type, uint16_t ep_mps)
{
    (void)pdev;
    (void)HAL_PCD_EP_Open(&hpcd_USB_FS, ep_addr, ep_mps, ep_type);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)pdev;
    (void)HAL_PCD_EP_Close(&hpcd_USB_FS, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_FlushEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)pdev;
    (void)HAL_PCD_EP_Flush(&hpcd_USB_FS, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_StallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)pdev;
    (void)HAL_PCD_EP_SetStall(&hpcd_USB_FS, ep_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_ClearStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)pdev;
    (void)HAL_PCD_EP_ClrStall(&hpcd_USB_FS, ep_addr);
    return USBD_OK;
}

uint8_t USBD_LL_IsStallEP(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    PCD_HandleTypeDef *hpcd = (PCD_HandleTypeDef *)pdev->pData;

    if ((ep_addr & 0x80U) == 0x80U) {
        return hpcd->IN_ep[ep_addr & 0x7FU].is_stall;
    }
    return hpcd->OUT_ep[ep_addr & 0x7FU].is_stall;
}

USBD_StatusTypeDef USBD_LL_SetUSBAddress(USBD_HandleTypeDef *pdev, uint8_t dev_addr)
{
    (void)pdev;
    (void)HAL_PCD_SetAddress(&hpcd_USB_FS, dev_addr);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                    uint8_t *pbuf, uint32_t size)
{
    (void)pdev;
    (void)HAL_PCD_EP_Transmit(&hpcd_USB_FS, ep_addr, pbuf, size);
    return USBD_OK;
}

USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *pdev, uint8_t ep_addr,
                                          uint8_t *pbuf, uint32_t size)
{
    (void)pdev;
    (void)HAL_PCD_EP_Receive(&hpcd_USB_FS, ep_addr, pbuf, size);
    return USBD_OK;
}

uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *pdev, uint8_t ep_addr)
{
    (void)pdev;
    return HAL_PCD_EP_GetRxCount(&hpcd_USB_FS, ep_addr);
}

void USBD_LL_Delay(uint32_t Delay)
{
    HAL_Delay(Delay);
}

