#include "board_tk75.h"
#include "encoder.h"
#include "tk75_pins.h"
#include "at32f402_405.h"
#include "tusb.h"

#define ESC_BOOTMAGIC_DELTA_ADC  400U
#define ADC_CH_ESC_PC0           ADC_CHANNEL_10
#define AT32_UID_BASE_ADDR       0x1FFFF7E8UL

static volatile uint32_t g_ms_ticks       = 0U;
static board_scan_cb_t   g_scan_cb        = NULL;
static uint16_t          g_scan_period_us = 125U;
static bool              g_usb_phy_on     = false;

static void configure_system_clocks_216mhz(void)
{
  /* 1.3 V core LDO required above 168 MHz on AT32F405 */
  crm_periph_clock_enable(CRM_PWC_PERIPH_CLOCK, TRUE);
  pwc_ldo_output_voltage_set(PWC_LDO_OUTPUT_1V3);

  /* 6 wait cycles at 216 MHz */
  flash_psr_set(FLASH_WAIT_CYCLE_6);

  crm_clock_source_enable(CRM_CLOCK_SOURCE_HEXT, TRUE);
  while (crm_hext_stable_wait() == ERROR) {
  }

  /* PLL = 12 MHz * 72 / (1 * 4) = 216 MHz; PLLU = 864 MHz / 18 = 48 MHz */
  crm_pll_config(CRM_PLL_SOURCE_HEXT, 72U, 1U, CRM_PLL_FP_4);
  crm_pllu_div_set(CRM_PLL_FU_18);
  crm_clock_source_enable(CRM_CLOCK_SOURCE_PLL, TRUE);
  while (crm_flag_get(CRM_PLL_STABLE_FLAG) != SET) {
  }

  /* Enable 48 MHz PLLU output and route it to the USB clock tree */
  crm_pllu_output_set(TRUE);
  while (crm_flag_get(CRM_PLLU_STABLE_FLAG) != SET) {
  }
  crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_PLLU);

  crm_ahb_div_set(CRM_AHB_DIV_1);
  crm_apb2_div_set(CRM_APB2_DIV_1);
  crm_apb1_div_set(CRM_APB1_DIV_2);

  crm_auto_step_mode_enable(TRUE);
  crm_sysclk_switch(CRM_SCLK_PLL);
  while (crm_sysclk_switch_status_get() != CRM_SCLK_PLL) {
  }
  crm_auto_step_mode_enable(FALSE);

  system_core_clock_update();
}

static void init_power_pins(void)
{
  gpio_init_type gpio;

  crm_periph_clock_enable(CRM_GPIOA_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOF_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&gpio);
  gpio.gpio_mode           = GPIO_MODE_OUTPUT;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_pull           = GPIO_PULL_NONE;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;

  /* Start with rails off until subsystems finish initializing */
  gpio_bits_reset(TK75_RGB_PWR_PORT, TK75_RGB_PWR_PIN);
  gpio.gpio_pins = TK75_RGB_PWR_PIN;
  gpio_init(TK75_RGB_PWR_PORT, &gpio);

  gpio_bits_reset(TK75_TFT_PWR_PORT, TK75_TFT_PWR_PIN);
  gpio.gpio_pins = TK75_TFT_PWR_PIN;
  gpio_init(TK75_TFT_PWR_PORT, &gpio);
}

static void init_microsecond_timer(void)
{
  /* APB1 = 108 MHz, timer clock = 216 MHz -> div 216 = 1 MHz (1 us/tick) */
  crm_periph_clock_enable(CRM_TMR3_PERIPH_CLOCK, TRUE);
  tmr_base_init(TMR3, 0xFFFFU, 215U);
  tmr_cnt_dir_set(TMR3, TMR_COUNT_UP);
  tmr_counter_enable(TMR3, TRUE);
}

static bool read_adc1_pc0_once(uint16_t *out)
{
  adc_ordinary_software_trigger_enable(ADC1, TRUE);
  for (uint32_t i = 0U; i < 200000U; i++) {
    if (adc_flag_get(ADC1, ADC_CCE_FLAG) != RESET) {
      *out = adc_ordinary_conversion_data_get(ADC1);
      return true;
    }
  }
  return false;
}

static void check_esc_bootmagic(void)
{
  gpio_init_type gpio;
  adc_base_config_type adc_cfg;

  gpio_default_para_init(&gpio);
  gpio.gpio_mode = GPIO_MODE_ANALOG;
  gpio.gpio_pins = GPIO_PINS_0; /* PC0: col 0 */
  gpio_init(GPIOC, &gpio);

  /* Drive all matrix row rails low first so unpowered rows cannot float PC0 */
  gpio.gpio_mode           = GPIO_MODE_OUTPUT;
  gpio.gpio_out_type       = GPIO_OUTPUT_PUSH_PULL;
  gpio.gpio_drive_strength = GPIO_DRIVE_STRENGTH_STRONGER;
  gpio.gpio_pins           = TK75_ROW_C_MASK;
  gpio_init(GPIOC, &gpio);
  gpio_bits_reset(GPIOC, TK75_ROW_C_MASK);
  gpio.gpio_pins = TK75_ROW_A_MASK;
  gpio_init(GPIOA, &gpio);
  gpio_bits_reset(GPIOA, TK75_ROW_A_MASK);

  crm_periph_clock_enable(CRM_ADC1_PERIPH_CLOCK, TRUE);
  adc_clock_div_set(ADC_DIV_8); /* 216 MHz / 8 = 27 MHz */

  adc_base_default_para_init(&adc_cfg);
  adc_cfg.sequence_mode           = FALSE;
  adc_cfg.repeat_mode             = FALSE;
  adc_cfg.data_align              = ADC_RIGHT_ALIGNMENT;
  adc_cfg.ordinary_channel_length = 1U;
  adc_base_config(ADC1, &adc_cfg);
  adc_ordinary_channel_set(ADC1, ADC_CH_ESC_PC0, 1U, ADC_SAMPLETIME_28_5);
  adc_ordinary_conversion_trigger_set(ADC1, ADC12_ORDINARY_TRIG_SOFTWARE, TRUE);
  adc_enable(ADC1, TRUE);
  adc_calibration_init(ADC1);
  while (adc_calibration_init_status_get(ADC1) == SET) {
  }
  adc_calibration_start(ADC1);
  while (adc_calibration_status_get(ADC1) == SET) {
  }

  /* Power row 0 (PC6 = Esc), settle, flush S/H capacitor, then average 4 reads */
  gpio_bits_set(GPIOC, GPIO_PINS_6);
  board_delay_us(100U);
  uint16_t sample = 0U;
  bool ok = read_adc1_pc0_once(&sample);
  uint32_t sum = 0U;
  for (uint8_t i = 0U; ok && (i < 4U); i++) {
    ok = read_adc1_pc0_once(&sample);
    sum += sample;
  }
  gpio_bits_reset(GPIOC, GPIO_PINS_6);
  if (!ok) {
    adc_enable(ADC1, FALSE);
    return;
  }
  uint16_t raw_on = (uint16_t)(sum >> 2U);
  adc_enable(ADC1, FALSE);

  /* Hall switches pull ADC lower when pressed; compare against factory rest baseline */
  const uint16_t *rest_tbl = (const uint16_t *)TK75_FACTORY_CAL_BASE_ADDR;
  const uint8_t  *magic    = (const uint8_t *)(TK75_FACTORY_CAL_BASE_ADDR + 0x7FEU);
  bool pressed = false;
  if (magic[0] == 0x55U && magic[1] == 0xAAU && rest_tbl[0] > 1200U && rest_tbl[0] < 3900U) {
    pressed = (raw_on >= 200U) && (rest_tbl[0] > (uint16_t)(raw_on + ESC_BOOTMAGIC_DELTA_ADC));
  } else {
    pressed = (raw_on >= 200U) && (raw_on < 1900U);
  }
  if (pressed) {
    board_enter_bootloader();
  }
}

void board_init(void)
{
  nvic_priority_group_config(NVIC_PRIORITY_GROUP_4);
  configure_system_clocks_216mhz();
  SysTick_Config(system_core_clock / 1000U);
  init_microsecond_timer();
  init_power_pins();
  check_esc_bootmagic();
}

void board_usb_otghs_init(void)
{
  /* Ensure 48 MHz USB PLLU output and 12 MHz HEXT PHY reference are active */
  crm_pllu_output_set(TRUE);
  while (crm_flag_get(CRM_PLLU_STABLE_FLAG) != SET) {
  }
  crm_usb_clock_source_select(CRM_USB_CLOCK_SOURCE_PLLU);
  crm_usb_phy12_clock_select(CRM_USB_PHY12_CLOCK_HEXT_DIV_1);

  /* Reset OTGHS core cleanly in case bootloader left active controller state */
  crm_periph_clock_enable(CRM_OTGHS_PERIPH_CLOCK, TRUE);
  crm_periph_reset(CRM_OTGHS_PERIPH_RESET, TRUE);
  crm_periph_reset(CRM_OTGHS_PERIPH_RESET, FALSE);

  /* Disable VBUS sensing (GCCFG bit 21) and power on transceiver (bit 16) */
  *(__IO uint32_t *)0x40040E00UL &= ~0x01UL;
  *(__IO uint32_t *)0x40040038UL |= (1UL << 21) | (1UL << 16);
  board_delay_us(1000U);

  nvic_irq_enable(OTGHS_IRQn, 1U, 0U);
  g_usb_phy_on = true;
}

void board_start_scan_timer(board_scan_cb_t cb)
{
  g_scan_cb        = cb;
  g_scan_period_us = 125U;

  /* APB1 timer clock = 216 MHz -> div 216 = 1 MHz, PR = 124 -> 125 us (8 kHz) */
  crm_periph_clock_enable(CRM_TMR6_PERIPH_CLOCK, TRUE);
  tmr_base_init(TMR6, 124U, 215U);
  tmr_interrupt_enable(TMR6, TMR_OVF_INT, TRUE);
  nvic_irq_enable(TMR6_GLOBAL_IRQn, 2U, 0U);
  tmr_counter_enable(TMR6, TRUE);
}

void board_set_scan_period_us(uint16_t period_us)
{
  if (period_us < 125U) {
    period_us = 125U;
  } else if (period_us > 20000U) {
    period_us = 20000U;
  }
  if (period_us == g_scan_period_us) {
    return;
  }
  g_scan_period_us = period_us;
  tmr_counter_enable(TMR6, FALSE);
  tmr_period_value_set(TMR6, (uint32_t)(period_us - 1U));
  tmr_counter_value_set(TMR6, 0U);
  tmr_counter_enable(TMR6, TRUE);
}

void board_set_scan_rate_sleep(bool slow)
{
  board_set_scan_period_us(slow ? 5000U : 125U);
}

void board_set_scan_timer_enabled(bool enabled)
{
  tmr_counter_enable(TMR6, enabled ? TRUE : FALSE);
}

uint32_t board_millis(void)
{
  return g_ms_ticks;
}

uint32_t board_micros(void)
{
  uint32_t ms1 = g_ms_ticks;
  uint32_t val = SysTick->VAL;
  uint32_t ms2 = g_ms_ticks;

  if (ms1 != ms2) {
    val = SysTick->VAL;
  }
  uint32_t ticks_per_us = system_core_clock / 1000000U;
  uint32_t elapsed      = (SysTick->LOAD >= val) ? (SysTick->LOAD - val) : 0U;
  return (ms2 * 1000U) + (elapsed / ticks_per_us);
}

void board_delay_us(uint32_t us)
{
  uint16_t start = (uint16_t)tmr_counter_value_get(TMR3);
  while (us > 0U) {
    uint16_t step = (us > 30000U) ? 30000U : (uint16_t)us;
    while ((uint16_t)((uint16_t)tmr_counter_value_get(TMR3) - start) < step) {
    }
    start += step;
    us -= step;
  }
}

void board_set_rgb_power(bool on)
{
  if (on) {
    gpio_bits_set(TK75_RGB_PWR_PORT, TK75_RGB_PWR_PIN);
  } else {
    gpio_bits_reset(TK75_RGB_PWR_PORT, TK75_RGB_PWR_PIN);
  }
}

void board_set_tft_power(bool on)
{
  if (on) {
    gpio_bits_set(TK75_TFT_PWR_PORT, TK75_TFT_PWR_PIN);
  } else {
    gpio_bits_reset(TK75_TFT_PWR_PORT, TK75_TFT_PWR_PIN);
  }
}

void board_usb_set_connect(bool connected)
{
  if (!g_usb_phy_on) {
    return;
  }
  /* OTGHS1 DCTL (0x40040804) bit 1 = SftDiscon (1=detach pull-up, 0=attach) */
  if (connected) {
    *(__IO uint32_t *)0x40040804UL &= ~0x02UL;
  } else {
    *(__IO uint32_t *)0x40040804UL |= 0x02UL;
  }
}

void board_usb_set_phy_power(bool on)
{
  if (on) {
    if (!g_usb_phy_on) {
      board_usb_otghs_init();
    }
    return;
  }

  /* Match Artery reduce_power_consumption(): stop OTGHS PHY + 48 MHz PLLU when wireless. */
  nvic_irq_disable(OTGHS_IRQn);
  if ((CRM->ahben1 & (1UL << 29U)) == 0U) {
    crm_periph_clock_enable(CRM_OTGHS_PERIPH_CLOCK, TRUE);
  }
  *(__IO uint32_t *)0x40040804UL |= 0x02UL;
  *(__IO uint32_t *)0x40040038UL |= (1UL << 22U);
  *(__IO uint32_t *)0x40040E00UL |= 0x01UL;
  *(__IO uint32_t *)0x40040038UL &= ~(1UL << 16U);
  crm_periph_clock_enable(CRM_OTGHS_PERIPH_CLOCK, FALSE);
  crm_pllu_output_set(FALSE);
  g_usb_phy_on = false;
}

void board_wfi(void)
{
  __WFI();
}

__attribute__((noreturn)) void board_enter_bootloader(void)
{
  /* Trigger USB soft disconnect if active so host re-enumerates DFU cleanly */
  if (g_usb_phy_on) {
    *(__IO uint32_t *)0x40040804UL |= 0x02UL;
    board_delay_us(20000U);
  }

  __disable_irq();
  flash_unlock();
  flash_sector_erase(TK75_BOOTLOADER_MAGIC_ADDR);
  flash_word_program(TK75_BOOTLOADER_MAGIC_ADDR, TK75_BOOTLOADER_MAGIC_VAL);
  flash_lock();

  NVIC_SystemReset();
  while (1) {
  }
}

void board_get_uid_bytes(uint8_t out_uid[12])
{
  const uint8_t *uid = (const uint8_t *)AT32_UID_BASE_ADDR;
  for (uint32_t i = 0U; i < 12U; i++) {
    out_uid[i] = uid[i];
  }
}

void SysTick_Handler(void)
{
  g_ms_ticks++;
  encoder_hw_sample_isr();
}

void TMR6_GLOBAL_IRQHandler(void)
{
  if (tmr_interrupt_flag_get(TMR6, TMR_OVF_FLAG) != RESET) {
    tmr_flag_clear(TMR6, TMR_OVF_FLAG);
    if (g_scan_cb != NULL) {
      g_scan_cb();
    }
  }
}

void OTGHS_IRQHandler(void)
{
  tud_int_handler(BOARD_TUD_RHPORT);
}
