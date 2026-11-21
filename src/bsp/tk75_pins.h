#ifndef TK75_PINS_H
#define TK75_PINS_H

#include <stdint.h>

/* Flash partitioning (256 KB AT32F405RCT7) */
#define TK75_BOOTLOADER_MAGIC_ADDR  0x08004800UL
#define TK75_BOOTLOADER_MAGIC_VAL   0x55AA55AAUL
#define TK75_BOOT_HDR_ADDR          0x08005000UL
#define TK75_APP_VTOR_ADDR          0x08005200UL
#define TK75_EEPROM_BASE_ADDR       0x0803C000UL
#define TK75_EEPROM_SIZE_BYTES      0x4000UL /* 16 KB (4 x 4 KB wear-leveled slots) */
#define TK75_EEPROM_SLOT_BYTES      4096UL   /* 2 x 2 KB AT32 flash sectors per slot */
#define TK75_FLASH_SECTOR_BYTES     2048UL

/* Stock factory Hall calibration pages (never erased by firmware) */
#define TK75_FACTORY_CAL_BASE_ADDR  0x08032000UL
#define TK75_FACTORY_CAL_SPAN_ADDR  0x08032800UL
#define TK75_FACTORY_CAL_FLAGS_ADDR 0x08033000UL

/* Matrix dimensions */
#define TK75_MATRIX_ROWS            6U
#define TK75_HALL_COLS              15U
#define TK75_VIA_COLS               16U /* Col 15 holds encoder virtual positions */
#define TK75_HALL_SLOTS             (TK75_MATRIX_ROWS * TK75_HALL_COLS)
#define TK75_TOTAL_LEDS             90U /* 0..81 per-key + 82..89 sidelight */
#define TK75_KEY_LEDS               82U
#define TK75_SIDE_LEDS              8U

/* Hall MUX active-HIGH row drive pins */
/* Row 0: PC6, Row 1: PC7, Row 2: PC8, Row 3: PC9, Row 4: PA8, Row 5: PA9 */
#define TK75_ROW_C_MASK             (GPIO_PINS_6 | GPIO_PINS_7 | GPIO_PINS_8 | GPIO_PINS_9)
#define TK75_ROW_A_MASK             (GPIO_PINS_8 | GPIO_PINS_9)

/* Battery ADC is multiplexed on Hall slot (row=5, col=4) -> PA0 (ADC1 CH0) */
#define TK75_VBAT_ROW               5U
#define TK75_VBAT_COL               4U

/* WS2812B LED chain: SPI2 MOSI on PA10 (AF5), power enable PB13 (active-HIGH) */
#define TK75_RGB_SPI                SPI2
#define TK75_RGB_PORT               GPIOA
#define TK75_RGB_PIN                GPIO_PINS_10
#define TK75_RGB_PIN_SRC            GPIO_PINS_SOURCE10
#define TK75_RGB_PIN_MUX            GPIO_MUX_5
#define TK75_RGB_PWR_PORT           GPIOB
#define TK75_RGB_PWR_PIN            GPIO_PINS_13

/* TFT smart display MCU link: UART7 @ 921600 8N1 (PF7 TX / PF6 RX, AF9), power PA12 (active-HIGH) */
#define TK75_TFT_UART               UART7
#define TK75_TFT_BAUD               921600U
#define TK75_TFT_GPIO_PORT          GPIOF
#define TK75_TFT_TX_PIN             GPIO_PINS_7
#define TK75_TFT_RX_PIN             GPIO_PINS_6
#define TK75_TFT_TX_SRC             GPIO_PINS_SOURCE7
#define TK75_TFT_RX_SRC             GPIO_PINS_SOURCE6
#define TK75_TFT_PIN_MUX            GPIO_MUX_9
#define TK75_TFT_PWR_PORT           GPIOA
#define TK75_TFT_PWR_PIN            GPIO_PINS_12

/* Rotary encoder: PC11 (A), PC10 (B), PC12 (Push active-LOW, pull-up) */
#define TK75_ENC_PORT               GPIOC
#define TK75_ENC_A_PIN              GPIO_PINS_11
#define TK75_ENC_B_PIN              GPIO_PINS_10
#define TK75_ENC_BTN_PIN            GPIO_PINS_12

/* Mode / VBUS / Charger switch inputs */
#define TK75_MODE_B2_PIN            GPIO_PINS_2   /* GPIOB: 3-way slider BT=0 */
#define TK75_MODE_B10_PIN           GPIO_PINS_10  /* GPIOB: 3-way slider 2.4G=0 */
#define TK75_VBUS_C13_PIN           GPIO_PINS_13  /* GPIOC: active LOW USB VBUS detect */
#define TK75_CHG_STAT_C14_PIN       GPIO_PINS_14  /* GPIOC: charger STAT (LOW=done, HIGH=charging when VBUS=0) */
#define TK75_MODE_C13_PIN           TK75_VBUS_C13_PIN
#define TK75_MODE_C14_PIN           TK75_CHG_STAT_C14_PIN

/* RF / BLE Transceiver on SPI3 (AF6) */
#define TK75_RF_SPI                 SPI3
#define TK75_RF_SPI_CLK             CRM_SPI3_PERIPH_CLOCK
#define TK75_RF_SPI_PORT            GPIOB
#define TK75_RF_SCK_PIN             GPIO_PINS_3   /* PB3 AF6 */
#define TK75_RF_SCK_SRC             GPIO_PINS_SOURCE3
#define TK75_RF_MISO_PIN            GPIO_PINS_4   /* PB4 AF6 */
#define TK75_RF_MISO_SRC            GPIO_PINS_SOURCE4
#define TK75_RF_MOSI_PIN            GPIO_PINS_5   /* PB5 AF6 */
#define TK75_RF_MOSI_SRC            GPIO_PINS_SOURCE5
#define TK75_RF_CS_PORT             GPIOA
#define TK75_RF_CS_PIN              GPIO_PINS_15  /* PA15 soft CS */
#define TK75_RF_IRQ_PORT            GPIOD
#define TK75_RF_IRQ_PIN             GPIO_PINS_2   /* PD2 active-LOW ready/data IRQ */
#define TK75_RF_CTRL_PORT           GPIOB
#define TK75_RF_CTRL_PIN            GPIO_PINS_12  /* PB12 run output PP, sleep floating */
#define TK75_RF_DMA                 DMA1
#define TK75_RF_DMA_CLK             CRM_DMA1_PERIPH_CLOCK
#define TK75_RF_RX_DMA_CH           DMA1_CHANNEL3
#define TK75_RF_TX_DMA_CH           DMA1_CHANNEL2
#define TK75_RF_RX_DMAMUX_CH        DMA1MUX_CHANNEL3
#define TK75_RF_TX_DMAMUX_CH        DMA1MUX_CHANNEL2
#define TK75_RF_RX_DMAMUX_REQ       DMAMUX_DMAREQ_ID_SPI3_RX
#define TK75_RF_TX_DMAMUX_REQ       DMAMUX_DMAREQ_ID_SPI3_TX
#define TK75_RF_TX_DMA_IRQn         DMA1_Channel2_IRQn

#endif /* TK75_PINS_H */
