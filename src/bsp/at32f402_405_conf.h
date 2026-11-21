#ifndef AT32F402_405_CONF_H
#define AT32F402_405_CONF_H

#ifdef __cplusplus
extern "C" {
#endif

#if !defined(HEXT_VALUE)
#define HEXT_VALUE ((uint32_t)12000000)
#endif

#define HEXT_STARTUP_TIMEOUT ((uint16_t)0x3000)
#define HICK_VALUE           ((uint32_t)8000000)
#define LEXT_VALUE           ((uint32_t)32768)

#define ADC_MODULE_ENABLED
#define CRM_MODULE_ENABLED
#define DEBUG_MODULE_ENABLED
#define DMA_MODULE_ENABLED
#define EXINT_MODULE_ENABLED
#define FLASH_MODULE_ENABLED
#define GPIO_MODULE_ENABLED
#define MISC_MODULE_ENABLED
#define PWC_MODULE_ENABLED
#define SCFG_MODULE_ENABLED
#define SPI_MODULE_ENABLED
#define TMR_MODULE_ENABLED
#define USART_MODULE_ENABLED
#define USB_MODULE_ENABLED
#define WDT_MODULE_ENABLED

#include "at32f402_405_adc.h"
#include "at32f402_405_crm.h"
#include "at32f402_405_debug.h"
#include "at32f402_405_dma.h"
#include "at32f402_405_exint.h"
#include "at32f402_405_flash.h"
#include "at32f402_405_gpio.h"
#include "at32f402_405_misc.h"
#include "at32f402_405_pwc.h"
#include "at32f402_405_scfg.h"
#include "at32f402_405_spi.h"
#include "at32f402_405_tmr.h"
#include "at32f402_405_usart.h"
#include "at32f402_405_usb.h"
#include "at32f402_405_wdt.h"

#ifdef __cplusplus
}
#endif

#endif /* AT32F402_405_CONF_H */
