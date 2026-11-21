#include "encoder.h"

#ifdef AT32F405RCT7
#include "at32f402_405.h"
#include "tk75_pins.h"
static encoder_state_t  g_enc_hw;
static volatile bool    g_enc_initialized   = false;
static volatile uint8_t g_enc_ab_raw        = 0U;
static volatile uint8_t g_enc_ab_cnt        = 0U;
static volatile uint8_t g_enc_ab_stable     = 0U;
static volatile int8_t  g_enc_delta_pending = 0;
#endif

#define ENC_BTN_DEBOUNCE_TICKS 5U
#define ENC_AB_DEBOUNCE_TICKS  2U
#define ENC_DELTA_CLAMP        64

enum {
  ENC_DIR_NONE = 0U,
  ENC_DIR_CW   = 1U,
  ENC_DIR_CCW  = 2U,
};

void encoder_state_init(encoder_state_t *st, bool a_high, bool b_high)
{
  st->prev_ab      = (uint8_t)((a_high ? 1U : 0U) | (b_high ? 2U : 0U));
  st->seq          = ENC_DIR_NONE;
  st->accum_steps  = 0;
  st->btn_debounce = 0U;
  st->btn_state    = false;
}

int8_t encoder_state_step(encoder_state_t *st, uint8_t ab_bits, bool btn_active)
{
  if (btn_active != st->btn_state) {
    st->btn_debounce++;
    if (st->btn_debounce >= ENC_BTN_DEBOUNCE_TICKS) {
      st->btn_state    = btn_active;
      st->btn_debounce = 0U;
    }
  } else {
    st->btn_debounce = 0U;
  }

  uint8_t curr = ab_bits & 0x03U;
  if (curr == st->prev_ab) {
    return 0;
  }

  /* Match stock FUN_0801dff8 half-period state machine: latch direction on detent exit,
   * emit one step only on matching completion into 00/11 so contact bounce cannot flip polarity. */
  uint8_t trans = (uint8_t)(((st->prev_ab & 0x03U) << 2U) | curr);
  st->prev_ab = curr;
  st->accum_steps = 0;

  switch (trans) {
    case 0x01U: /* 00 -> 01 */
    case 0x0EU: /* 11 -> 10 */
      st->seq = ENC_DIR_CW;
      return 0;

    case 0x02U: /* 00 -> 10 */
    case 0x0DU: /* 11 -> 01 */
      st->seq = ENC_DIR_CCW;
      return 0;

    case 0x07U: /* 01 -> 11 */
    case 0x08U: /* 10 -> 00 */
      if (st->seq == ENC_DIR_CW) {
        st->seq = ENC_DIR_NONE;
        return 1;
      }
      st->seq = ENC_DIR_NONE;
      return 0;

    case 0x0BU: /* 10 -> 11 */
    case 0x04U: /* 01 -> 00 */
      if (st->seq == ENC_DIR_CCW) {
        st->seq = ENC_DIR_NONE;
        return -1;
      }
      st->seq = ENC_DIR_NONE;
      return 0;

    default:
      st->seq = ENC_DIR_NONE;
      return 0;
  }
}

#ifdef AT32F405RCT7
static uint8_t read_raw_ab(void)
{
  uint8_t a = (gpio_input_data_bit_read(TK75_ENC_PORT, TK75_ENC_A_PIN) != RESET) ? 1U : 0U;
  uint8_t b = (gpio_input_data_bit_read(TK75_ENC_PORT, TK75_ENC_B_PIN) != RESET) ? 2U : 0U;
  return (uint8_t)(a | b);
}

void encoder_hw_init(void)
{
  gpio_init_type gpio;
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);

  gpio_default_para_init(&gpio);
  gpio.gpio_mode = GPIO_MODE_INPUT;
  gpio.gpio_pull = GPIO_PULL_UP;
  gpio.gpio_pins = TK75_ENC_A_PIN | TK75_ENC_B_PIN | TK75_ENC_BTN_PIN;
  gpio_init(TK75_ENC_PORT, &gpio);

  uint8_t raw = read_raw_ab();
  g_enc_ab_raw        = raw;
  g_enc_ab_stable     = raw;
  g_enc_ab_cnt        = 0U;
  g_enc_delta_pending = 0;
  encoder_state_init(&g_enc_hw, (raw & 1U) != 0U, (raw & 2U) != 0U);
  g_enc_initialized = true;
}

void encoder_hw_sample_isr(void)
{
  if (!g_enc_initialized) {
    return;
  }

  uint8_t raw_ab = read_raw_ab();
  if (raw_ab == g_enc_ab_stable) {
    g_enc_ab_cnt = 0U;
  } else if (raw_ab == g_enc_ab_raw) {
    if (++g_enc_ab_cnt >= ENC_AB_DEBOUNCE_TICKS) {
      g_enc_ab_stable = raw_ab;
      g_enc_ab_cnt    = 0U;
    }
  } else {
    g_enc_ab_raw = raw_ab;
    g_enc_ab_cnt = 1U;
  }

  bool btn_low = (gpio_input_data_bit_read(TK75_ENC_PORT, TK75_ENC_BTN_PIN) == RESET);
  int8_t d = encoder_state_step(&g_enc_hw, g_enc_ab_stable, btn_low);
  if (d != 0) {
    int16_t next = (int16_t)g_enc_delta_pending + d;
    if (next > ENC_DELTA_CLAMP) {
      next = ENC_DELTA_CLAMP;
    } else if (next < -ENC_DELTA_CLAMP) {
      next = -ENC_DELTA_CLAMP;
    }
    g_enc_delta_pending = (int8_t)next;
  }
}

encoder_events_t encoder_hw_poll(void)
{
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  encoder_events_t ev;
  ev.delta            = g_enc_delta_pending;
  g_enc_delta_pending = 0;
  ev.btn_pressed      = g_enc_hw.btn_state;
  __set_PRIMASK(primask);
  return ev;
}

uint8_t encoder_hw_diag_bits(void)
{
  uint8_t ab      = read_raw_ab();
  bool    btn_low = (gpio_input_data_bit_read(TK75_ENC_PORT, TK75_ENC_BTN_PIN) == RESET);
  return (uint8_t)(ab | (btn_low ? 0x04U : 0U) | (g_enc_hw.btn_state ? 0x08U : 0U));
}
#endif
