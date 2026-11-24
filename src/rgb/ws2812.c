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

#define WS_NIB(b3, b2, b1, b0) \
  (((uint32_t)(b3)) | ((uint32_t)(b2) << 8U) | ((uint32_t)(b1) << 16U) | ((uint32_t)(b0) << 24U))

/* 16-entry nibble LUT: encodes 4 WS2812 bits in a single 32-bit store (8x faster than bit loop) */
static uint32_t const k_nibble_lut[16] = {
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ONE),
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ONE),
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ONE),
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ONE),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ZERO, WS2812_SYM_ONE),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ONE,  WS2812_SYM_ONE),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ZERO, WS2812_SYM_ONE),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ZERO),
  WS_NIB(WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ONE,  WS2812_SYM_ONE),
};

static uint8_t       g_spi_tx_buf[WS2812_DMA_BUF_BYTES] __attribute__((aligned(4)));
static volatile bool g_dma_busy    = false;
static bool          g_bus_powered = false;

static inline void encode_byte(uint8_t val, uint8_t *dst)
{
  uint32_t hi = k_nibble_lut[val >> 4U];
  uint32_t lo = k_nibble_lut[val & 0x0FU];
  memcpy(dst, &hi, 4U);
  memcpy(dst + 4U, &lo, 4U);
}

static void configure_spi2_and_pin(void)
{
  gpio_init_type gpio;
  spi_init_type spi;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, TRUE);

  /* PA10 = SPI2_MOSI (AF5), verified from A26065 in stock v506 firmware */
  gpio_default_para_init(&gpio);
  gpio.gpio_pins           = TK75_RGB_PIN;
  gpio.gpio_mode           = GPIO_MODE_MUX;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_pull           = GPIO_PULL_NONE;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio_init(TK75_RGB_PORT, &gpio);
  gpio_pin_mux_config(TK75_RGB_PORT, TK75_RGB_PIN_SRC, TK75_RGB_PIN_MUX);

  spi_i2s_reset(TK75_RGB_SPI);
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
}

void ws2812_set_bus_power(bool enabled)
{
  if (enabled == g_bus_powered) {
    return;
  }
  if (!enabled) {
    dma_channel_enable(DMA1_CHANNEL1, FALSE);
    dma_flag_clear(DMA1_FDT1_FLAG);
    g_dma_busy = false;
    spi_i2s_dma_transmitter_enable(TK75_RGB_SPI, FALSE);
    spi_enable(TK75_RGB_SPI, FALSE);
    crm_periph_clock_enable(CRM_SPI2_PERIPH_CLOCK, FALSE);

    /* Drive PA10 LOW push-pull so unpowered WS2812B LEDs cannot back-power through DIN */
    gpio_init_type gpio;
    gpio_default_para_init(&gpio);
    gpio.gpio_pins           = TK75_RGB_PIN;
    gpio.gpio_mode           = GPIO_MODE_OUTPUT;
    gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
    gpio.gpio_pull           = GPIO_PULL_NONE;
    gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_MODERATE;
    gpio_bits_reset(TK75_RGB_PORT, TK75_RGB_PIN);
    gpio_init(TK75_RGB_PORT, &gpio);
    g_bus_powered = false;
    return;
  }

  configure_spi2_and_pin();
  g_bus_powered = true;
}

void ws2812_init(void)
{
  dma_init_type dma;

  memset(g_spi_tx_buf, 0, sizeof(g_spi_tx_buf));
  g_dma_busy    = false;
  g_bus_powered = false;

  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);

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

  /* Drive DIN low initially; sync_power_rails() enables SPI2 + PB13 if lighting is active */
  g_bus_powered = true;
  ws2812_set_bus_power(false);
}

bool ws2812_show_async(rgb_grb_t const leds[RGB_LED_COUNT])
{
  if (!g_bus_powered || g_dma_busy || leds == NULL) {
    return false;
  }

  for (uint32_t i = 0U; i < RGB_LED_COUNT; i++) {
    uint32_t off = i * WS2812_BITS_PER_LED;
    encode_byte(leds[i].g, &g_spi_tx_buf[off]);
    encode_byte(leds[i].r, &g_spi_tx_buf[off + 8U]);
    encode_byte(leds[i].b, &g_spi_tx_buf[off + 16U]);
  }
  /* Tail reset bytes are already zeroed in ws2812_init() and never overwritten */

  g_dma_busy = true;
  dma_channel_enable(DMA1_CHANNEL1, FALSE);
  DMA1_CHANNEL1->maddr = (uint32_t)g_spi_tx_buf;
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
