#include "ws2812.h"
#include "board_tk75.h"
#include "tk75_pins.h"
#include "at32f402_405.h"
#include <string.h>

#define WS2812_BITS_PER_LED  24U
#define WS2812_RESET_BYTES   80U
#define WS2812_DMA_BUF_BYTES ((RGB_LED_COUNT * WS2812_BITS_PER_LED) + WS2812_RESET_BYTES)

/* At 6.75 MHz SPI clock (148 ns/bit), 8 bits = 1.185 us WS2812 symbol */
#define WS2812_SYM_ZERO      0xC0U /* ~296 ns high, ~889 ns low */
#define WS2812_SYM_ONE       0xF0U /* ~593 ns high, ~593 ns low */

static uint8_t       g_spi_tx_buf[WS2812_DMA_BUF_BYTES];
static volatile bool g_dma_busy = false;

static void encode_byte(uint8_t val, uint8_t *dst)
{
  for (uint8_t bit = 0U; bit < 8U; bit++) {
    dst[bit] = ((val & (0x80U >> bit)) != 0U) ? WS2812_SYM_ONE : WS2812_SYM_ZERO;
  }
}

void ws2812_init(void)
{
  gpio_init_type gpio;
  spi_init_type spi;
  dma_init_type dma;

  memset(g_spi_tx_buf, 0, sizeof(g_spi_tx_buf));

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);

  /* PA10 = SPI2_MOSI (AF5), verified from A26065 in stock v506 firmware */
  gpio_default_para_init(&gpio);
  gpio.gpio_pins           = TK75_RGB_PIN;
  gpio.gpio_mode           = GPIO_MODE_MUX;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_pull           = GPIO_PULL_NONE;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(TK75_RGB_PORT, &gpio);
  gpio_pin_mux_config(TK75_RGB_PORT, TK75_RGB_PIN_SRC, TK75_RGB_PIN_MUX);

  spi_default_para_init(&spi);
  spi.transmission_mode      = SPI_TRANSMIT_HALF_DUPLEX_TX;
  spi.master_slave_mode      = SPI_MODE_MASTER;
  spi.mclk_freq_division     = SPI_MCLK_DIV_16; /* APB1 108 MHz / 16 = 6.75 MHz */
  spi.first_bit_transmission = SPI_FIRST_BIT_MSB;
  spi.frame_bit_num          = SPI_FRAME_8BIT;
  spi.clock_polarity         = SPI_CLOCK_POLARITY_LOW;
  spi.clock_phase            = SPI_CLOCK_PHASE_1EDGE;
  spi.cs_mode_selection      = SPI_CS_SOFTWARE_MODE;
  spi_init(TK75_RGB_SPI, &spi);
  spi_i2s_dma_transmitter_enable(TK75_RGB_SPI, TRUE);
  spi_enable(TK75_RGB_SPI, TRUE);

  dma_reset(DMA1_CHANNEL1);
  dma_default_para_init(&dma);
  dma.buffer_size           = WS2812_DMA_BUF_BYTES;
  dma.direction             = DMA_DIR_MEMORY_TO_PERIPHERAL;
  dma.memory_base_addr      = (uint32_t)g_spi_tx_buf;
  dma.memory_data_width     = DMA_MEMORY_DATA_WIDTH_BYTE;
  dma.memory_inc_enable     = TRUE;
  dma.peripheral_base_addr  = (uint32_t)&(TK75_RGB_SPI->dt);
  dma.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_BYTE;
  dma.peripheral_inc_enable = FALSE;
  dma.loop_mode_enable      = FALSE;
  dma.priority              = DMA_PRIORITY_HIGH;
  dma_init(DMA1_CHANNEL1, &dma);

  dmamux_enable(DMA1, TRUE);
  dma_flexible_config(DMA1, DMA1MUX_CHANNEL1, DMAMUX_DMAREQ_ID_SPI2_TX);
  dma_interrupt_enable(DMA1_CHANNEL1, DMA_FDT_INT, TRUE);
  nvic_irq_enable(DMA1_Channel1_IRQn, 3U, 0U);

  /* Enable LED rail after MOSI & SPI2 are initialized */
  board_set_rgb_power(true);
}

bool ws2812_show_async(rgb_grb_t const leds[RGB_LED_COUNT])
{
  if (g_dma_busy || leds == NULL) {
    return false;
  }

  for (uint32_t i = 0U; i < RGB_LED_COUNT; i++) {
    uint32_t off = i * WS2812_BITS_PER_LED;
    encode_byte(leds[i].g, &g_spi_tx_buf[off]);
    encode_byte(leds[i].r, &g_spi_tx_buf[off + 8U]);
    encode_byte(leds[i].b, &g_spi_tx_buf[off + 16U]);
  }
  memset(&g_spi_tx_buf[RGB_LED_COUNT * WS2812_BITS_PER_LED], 0, WS2812_RESET_BYTES);

  g_dma_busy = true;
  dma_channel_enable(DMA1_CHANNEL1, FALSE);
  dma_data_number_set(DMA1_CHANNEL1, WS2812_DMA_BUF_BYTES);
  dma_channel_enable(DMA1_CHANNEL1, TRUE);
  return true;
}

bool ws2812_is_ready(void)
{
  return !g_dma_busy;
}

void DMA1_Channel1_IRQHandler(void)
{
  if (dma_interrupt_flag_get(DMA1_FDT1_FLAG) != RESET) {
    dma_flag_clear(DMA1_FDT1_FLAG);
    dma_channel_enable(DMA1_CHANNEL1, FALSE);
    g_dma_busy = false;
  }
}
