#include "adc_scanner_at32.h"
#include "mode_switches.h"
#include "board_tk75.h"
#include "tk75_pins.h"
#include "at32f402_405.h"
#include <stddef.h>

#define MUX_SETTLE_US         3U
#define TK75_VBAT_SAMPLE_MIN  600U
#define TK75_VBAT_SAMPLE_MAX  3000U

static adc_channel_select_type const k_col_channels[HALL_COLS] = {
  ADC_CHANNEL_10, /* Col  0: PC0 */
  ADC_CHANNEL_11, /* Col  1: PC1 */
  ADC_CHANNEL_12, /* Col  2: PC2 */
  ADC_CHANNEL_13, /* Col  3: PC3 */
  ADC_CHANNEL_0,  /* Col  4: PA0 (also VBAT on row 5) */
  ADC_CHANNEL_1,  /* Col  5: PA1 */
  ADC_CHANNEL_2,  /* Col  6: PA2 */
  ADC_CHANNEL_3,  /* Col  7: PA3 */
  ADC_CHANNEL_4,  /* Col  8: PA4 */
  ADC_CHANNEL_5,  /* Col  9: PA5 */
  ADC_CHANNEL_6,  /* Col 10: PA6 */
  ADC_CHANNEL_7,  /* Col 11: PA7 */
  ADC_CHANNEL_14, /* Col 12: PC4 */
  ADC_CHANNEL_15, /* Col 13: PC5 */
  ADC_CHANNEL_8,  /* Col 14: PB0 */
};

static uint16_t          g_frames[2][HALL_SLOT_COUNT];
static volatile uint8_t  g_write_idx   = 0U;
static volatile uint8_t  g_ready_idx   = 0U;
static volatile bool     g_frame_ready = false;
static volatile bool     g_scanning    = false;
static volatile uint8_t  g_cur_row     = 0U;
static volatile uint16_t g_vbat_filt   = 0U;
static volatile bool     g_vbat_primed = false;

static void mux_select_row(uint8_t row)
{
  GPIOC->clr = TK75_ROW_C_MASK;
  GPIOA->clr = TK75_ROW_A_MASK;

  switch (row) {
    case 0U: GPIOC->scr = GPIO_PINS_6; break;
    case 1U: GPIOC->scr = GPIO_PINS_7; break;
    case 2U: GPIOC->scr = GPIO_PINS_8; break;
    case 3U: GPIOC->scr = GPIO_PINS_9; break;
    case 4U: GPIOA->scr = GPIO_PINS_8; break;
    case 5U: GPIOA->scr = GPIO_PINS_9; break;
    default: break;
  }
}

static void start_row_dma(uint8_t row)
{
  mux_select_row(row);
  board_delay_us(MUX_SETTLE_US);

  dma_channel_enable(DMA1_CHANNEL6, FALSE);
  DMA1_CHANNEL6->maddr = (uint32_t)&g_frames[g_write_idx][(uint32_t)row * HALL_COLS];
  dma_data_number_set(DMA1_CHANNEL6, HALL_COLS);
  dma_channel_enable(DMA1_CHANNEL6, TRUE);
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
}

static void init_scanner_gpio(void)
{
  gpio_init_type gpio;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&gpio);
  gpio.gpio_mode = GPIO_MODE_ANALOG;
  gpio.gpio_pull = GPIO_PULL_NONE;

  gpio.gpio_pins = GPIO_PINS_0 | GPIO_PINS_1 | GPIO_PINS_2 | GPIO_PINS_3 |
                   GPIO_PINS_4 | GPIO_PINS_5 | GPIO_PINS_6 | GPIO_PINS_7;
  gpio_init(GPIOA, &gpio);

  gpio.gpio_pins = GPIO_PINS_0;
  gpio_init(GPIOB, &gpio);

  gpio.gpio_pins = GPIO_PINS_0 | GPIO_PINS_1 | GPIO_PINS_2 | GPIO_PINS_3 |
                   GPIO_PINS_4 | GPIO_PINS_5;
  gpio_init(GPIOC, &gpio);

  gpio_default_para_init(&gpio);
  gpio.gpio_mode           = GPIO_MODE_OUTPUT;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;

  gpio.gpio_pins = TK75_ROW_C_MASK;
  gpio_init(GPIOC, &gpio);

  gpio.gpio_pins = TK75_ROW_A_MASK;
  gpio_init(GPIOA, &gpio);

  mux_select_row(0xFFU);
}

static void init_adc_and_dma(void)
{
  dma_init_type dma_cfg;
  adc_base_config_type adc_cfg;

  crm_periph_clock_enable(CRM_DMA1_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);

  dma_reset(DMA1_CHANNEL6);
  dma_default_para_init(&dma_cfg);
  dma_cfg.buffer_size           = HALL_COLS;
  dma_cfg.direction             = DMA_DIR_PERIPHERAL_TO_MEMORY;
  dma_cfg.memory_base_addr      = (uint32_t)&g_frames[0][0];
  dma_cfg.memory_data_width     = DMA_MEMORY_DATA_WIDTH_HALFWORD;
  dma_cfg.memory_inc_enable     = TRUE;
  dma_cfg.peripheral_base_addr  = (uint32_t)&(ADC1->odt);
  dma_cfg.peripheral_data_width = DMA_PERIPHERAL_DATA_WIDTH_HALFWORD;
  dma_cfg.peripheral_inc_enable = FALSE;
  dma_cfg.loop_mode_enable      = FALSE;
  dma_cfg.priority              = DMA_PRIORITY_VERY_HIGH;
  dma_init(DMA1_CHANNEL6, &dma_cfg);

  dmamux_enable(DMA1, TRUE);
  dma_flexible_config(DMA1, DMA1MUX_CHANNEL6, DMAMUX_DMAREQ_ID_ADC1);
  dma_interrupt_enable(DMA1_CHANNEL6, DMA_FDT_INT, TRUE);
  nvic_irq_enable(DMA1_Channel6_IRQn, 1U, 0U);

  adc_reset(ADC1);
  adc_clock_div_set(ADC_DIV_8); /* 216 MHz / 8 = 27 MHz */
  adc_base_default_para_init(&adc_cfg);
  adc_cfg.sequence_mode           = TRUE;
  adc_cfg.repeat_mode             = FALSE;
  adc_cfg.data_align              = ADC_RIGHT_ALIGNMENT;
  adc_cfg.ordinary_channel_length = HALL_COLS;
  adc_base_config(ADC1, &adc_cfg);

  for (uint8_t col = 0U; col < HALL_COLS; col++) {
    adc_ordinary_channel_set(ADC1, k_col_channels[col], (uint8_t)(col + 1U), ADC_SAMPLETIME_7_5);
  }

  adc_ordinary_conversion_trigger_set(ADC1, ADC12_ORDINARY_TRIG_SOFTWARE, TRUE);
  adc_dma_mode_enable(ADC1, TRUE);
  adc_enable(ADC1, TRUE);
  adc_calibration_init(ADC1);
  while (adc_calibration_init_status_get(ADC1) == SET) {
  }
  adc_calibration_start(ADC1);
  while (adc_calibration_status_get(ADC1) == SET) {
  }
}

void adc_scanner_init(void)
{
  init_scanner_gpio();
  init_adc_and_dma();
}

void adc_scanner_start_frame_isr(void)
{
  if (g_scanning) {
    return;
  }
  g_scanning = true;
  g_cur_row  = 0U;
  start_row_dma(0U);
}

void adc_scanner_wait_idle(void)
{
  for (uint32_t spins = 0U; g_scanning && spins < 200000U; spins++) {
    __NOP();
  }
}

uint16_t const *adc_scanner_capture_blocking(void)
{
  adc_scanner_wait_idle();
  g_frame_ready = false;
  adc_scanner_start_frame_isr();
  while (!g_frame_ready) {
  }
  return adc_scanner_poll_frame();
}

uint16_t const *adc_scanner_poll_frame(void)
{
  if (!g_frame_ready) {
    return NULL;
  }
  g_frame_ready = false;
  return g_frames[g_ready_idx];
}

uint16_t adc_scanner_get_vbat_raw(void)
{
  return g_vbat_primed ? g_vbat_filt : 0U;
}

uint8_t adc_scanner_get_battery_percent(void)
{
  uint16_t v = adc_scanner_get_vbat_raw();
  if (v == 0U) {
    return 100U;
  }
  return tk75_battery_adc_to_pct(v);
}

void DMA1_Channel6_IRQHandler(void)
{
  if (dma_interrupt_flag_get(DMA1_FDT6_FLAG) == RESET) {
    return;
  }
  dma_flag_clear(DMA1_FDT6_FLAG);

  uint8_t finished_row = g_cur_row;
  g_cur_row++;
  if (g_cur_row < HALL_ROWS) {
    start_row_dma(g_cur_row);
    return;
  }

  mux_select_row(0xFFU);
  dma_channel_enable(DMA1_CHANNEL6, FALSE);

  if (finished_row == (HALL_ROWS - 1U)) {
    uint16_t vbat_raw = g_frames[g_write_idx][((uint32_t)TK75_VBAT_ROW * HALL_COLS) + TK75_VBAT_COL];
    if (vbat_raw >= TK75_VBAT_SAMPLE_MIN && vbat_raw <= TK75_VBAT_SAMPLE_MAX) {
      if (!g_vbat_primed) {
        g_vbat_filt   = vbat_raw;
        g_vbat_primed = true;
      } else {
        g_vbat_filt = (uint16_t)(((uint32_t)g_vbat_filt * 15U + vbat_raw) >> 4U);
      }
    }
  }

  g_ready_idx   = g_write_idx;
  g_write_idx  ^= 1U;
  g_frame_ready = true;
  g_scanning    = false;
}
