#include "mode_switches.h"
#include <string.h>

#ifdef AT32F405RCT7
#include "at32f402_405.h"
#include "tk75_pins.h"
static mode_switch_debouncer_t g_mode_db;
#endif

static bool state_equal(mode_switch_state_t a, mode_switch_state_t b)
{
  return (a.conn_mode == b.conn_mode) &&
         (a.vbus_present == b.vbus_present) &&
         (a.charging == b.charging);
}

mode_switch_state_t mode_switches_decode(bool pb2_high, bool pb10_high, bool pc13_high, bool pc14_high)
{
  mode_switch_state_t st;

  /* Verified against stock FUN_0801bf3c / FUN_08018a9c:
   * - PB2=0, PB10=1 -> Bluetooth (conn_mode = 2)
   * - PB2=1, PB10=0 -> 2.4 GHz  (conn_mode = 1)
   * - Else          -> USB Wired (conn_mode = 0)
   */
  if (!pb2_high && pb10_high) {
    st.conn_mode = TK75_CONN_BT;
  } else if (pb2_high && !pb10_high) {
    st.conn_mode = TK75_CONN_24G;
  } else {
    st.conn_mode = TK75_CONN_USB;
  }

  /* PC13 active-LOW USB VBUS detect; PC14 active-HIGH charger STAT when VBUS present */
  st.vbus_present = !pc13_high;
  st.charging     = st.vbus_present && pc14_high;
  return st;
}

mode_switch_state_t mode_switches_debounce_step(mode_switch_debouncer_t *db, mode_switch_state_t raw)
{
  if (db == NULL) {
    return raw;
  }
  if (!db->initialized) {
    db->stable      = raw;
    db->candidate   = raw;
    db->count       = 0U;
    db->initialized = true;
    return db->stable;
  }
  if (state_equal(raw, db->stable)) {
    db->candidate = raw;
    db->count     = 0U;
    return db->stable;
  }
  if (state_equal(raw, db->candidate)) {
    db->count++;
    if (db->count >= MODE_SWITCH_DEBOUNCE_TICKS) {
      db->stable = raw;
      db->count  = 0U;
    }
  } else {
    db->candidate = raw;
    db->count     = 1U;
  }
  return db->stable;
}

void mode_switches_init(void)
{
#ifdef AT32F405RCT7
  memset(&g_mode_db, 0, sizeof(g_mode_db));
  crm_periph_clock_enable(CRM_GPIOB_PERIPH_CLOCK, TRUE);
  crm_periph_clock_enable(CRM_GPIOC_PERIPH_CLOCK, TRUE);

  gpio_init_type gpio;
  gpio_default_para_init(&gpio);
  gpio.gpio_mode = GPIO_MODE_INPUT;
  gpio.gpio_pull = GPIO_PULL_UP;

  gpio.gpio_pins = TK75_MODE_B2_PIN | TK75_MODE_B10_PIN;
  gpio_init(GPIOB, &gpio);

  gpio.gpio_pins = TK75_VBUS_C13_PIN | TK75_CHG_STAT_C14_PIN;
  gpio_init(GPIOC, &gpio);
#endif
}

mode_switch_state_t mode_switches_read(void)
{
#ifdef AT32F405RCT7
  bool pb2_high  = (gpio_input_data_bit_read(GPIOB, TK75_MODE_B2_PIN) != RESET);
  bool pb10_high = (gpio_input_data_bit_read(GPIOB, TK75_MODE_B10_PIN) != RESET);
  bool pc13_high = (gpio_input_data_bit_read(GPIOC, TK75_VBUS_C13_PIN) != RESET);
  bool pc14_high = (gpio_input_data_bit_read(GPIOC, TK75_CHG_STAT_C14_PIN) != RESET);
  mode_switch_state_t raw = mode_switches_decode(pb2_high, pb10_high, pc13_high, pc14_high);
  return mode_switches_debounce_step(&g_mode_db, raw);
#else
  return mode_switches_decode(true, true, false, false);
#endif
}

#define TK75_VBAT_ADC_MIN_VALID 600U
#define TK75_VBAT_ADC_MAX_VALID 3000U

uint8_t tk75_battery_adc_to_pct(uint16_t adc)
{
  if (adc < 1110U) {
    return 1U;
  }
  if (adc <= 1195U) {
    uint32_t pct = 20U - (((uint32_t)(1195U - adc) * 20U) / 86U);
    return (pct < 1U) ? 1U : (uint8_t)pct;
  }
  if (adc <= 1432U) {
    uint32_t pct = 20U + (((uint32_t)(adc - 1195U) * 80U) / 238U);
    return (pct > 99U) ? 99U : (uint8_t)pct;
  }
  return 100U;
}

uint16_t tk75_battery_compensate_vbat(uint16_t raw_vbat_adc, tk75_vbat_load_t const *load)
{
  if (load == NULL || raw_vbat_adc < TK75_VBAT_ADC_MIN_VALID || raw_vbat_adc > TK75_VBAT_ADC_MAX_VALID) {
    return raw_vbat_adc;
  }
  uint32_t comp = 0U;
  if (load->tft_on) {
    uint8_t brt = (load->tft_brightness_pct > 100U) ? 100U : load->tft_brightness_pct;
    comp += 9U + ((uint32_t)brt / 15U);
  }
  if (load->rgb_rail_on) {
    uint32_t ch_sum = (load->rgb_channel_sum > 68850U) ? 68850U : load->rgb_channel_sum;
    comp += 4U + ((ch_sum * 38U) / 27000U);
  }
  uint32_t adj = (uint32_t)raw_vbat_adc + comp;
  return (adj > TK75_VBAT_ADC_MAX_VALID) ? TK75_VBAT_ADC_MAX_VALID : (uint16_t)adj;
}

void tk75_battery_gauge_init(tk75_battery_gauge_t *g)
{
  if (!g) {
    return;
  }
  memset(g, 0, sizeof(*g));
  g->pct = 100U;
}

bool tk75_battery_gauge_is_charging(tk75_battery_gauge_t const *g)
{
  return (g != NULL) && g->prev_vbus && g->prev_charging;
}

static void update_charge_complete(tk75_battery_gauge_t *g, uint16_t vbat_adc, uint8_t inst, bool charging)
{
  if (!charging) {
    if (inst >= 92U) {
      g->charge_complete = true;
      g->full_hold_ticks = TK75_BATT_FULL_CONFIRM_TICKS;
    } else {
      g->charge_complete = false;
      g->full_hold_ticks = 0U;
    }
    return;
  }

  /* Hardware PC14 stays HIGH while awake because MCU+Hall+TFT+RGB load exceeds charger I_term;
   * confirm true CV full charge once load-compensated cell voltage holds >= 4.205V steadily. */
  if (g->charge_complete) {
    if (vbat_adc < TK75_BATT_CV_DROP_ADC) {
      if (g->full_hold_ticks > 0U) {
        g->full_hold_ticks--;
      }
      if (g->full_hold_ticks == 0U) {
        g->charge_complete = false;
      }
    } else {
      g->full_hold_ticks = TK75_BATT_FULL_CONFIRM_TICKS;
    }
    return;
  }

  if (vbat_adc >= TK75_BATT_CV_FULL_ADC) {
    if (g->full_hold_ticks < TK75_BATT_FULL_CONFIRM_TICKS) {
      g->full_hold_ticks++;
    }
    if (g->full_hold_ticks >= TK75_BATT_FULL_CONFIRM_TICKS) {
      g->charge_complete = true;
    }
  } else {
    g->full_hold_ticks = 0U;
  }
}

uint8_t tk75_battery_gauge_step(tk75_battery_gauge_t *g, uint16_t vbat_adc, bool vbus_present, bool charging)
{
  if (!g) {
    return 100U;
  }
  bool hw_charging = vbus_present && charging;
  bool adc_valid   = (vbat_adc >= TK75_VBAT_ADC_MIN_VALID && vbat_adc <= TK75_VBAT_ADC_MAX_VALID);

  if (!adc_valid) {
    if (vbus_present && !hw_charging && vbat_adc < TK75_VBAT_ADC_MIN_VALID) {
      g->pct             = 100U;
      g->trend_ticks     = 0U;
      g->charge_complete = true;
      g->full_hold_ticks = TK75_BATT_FULL_CONFIRM_TICKS;
      g->prev_vbus       = vbus_present;
      g->prev_charging   = false;
    }
    return g->pct;
  }

  uint8_t inst = tk75_battery_adc_to_pct(vbat_adc);
  if (!g->initialized) {
    g->charge_complete    = (vbus_present && !hw_charging && inst >= 92U);
    g->full_hold_ticks    = g->charge_complete
                              ? TK75_BATT_FULL_CONFIRM_TICKS
                              : ((vbus_present && vbat_adc >= TK75_BATT_CV_FULL_ADC) ? 1U : 0U);
    bool eff_chg          = hw_charging && !g->charge_complete;
    g->pct                = eff_chg ? ((inst > 99U) ? 99U : inst)
                                    : ((vbus_present && inst >= 92U) ? 100U : inst);
    g->trend_ticks        = 0U;
    g->unplug_grace_ticks = 0U;
    g->initialized        = true;
    g->prev_vbus          = vbus_present;
    g->prev_charging      = eff_chg;
    return g->pct;
  }

  bool edge_unplug = (g->prev_vbus && !vbus_present);
  if (edge_unplug) {
    g->unplug_grace_ticks = TK75_BATT_UNPLUG_GRACE_TICKS;
    g->trend_ticks        = 0U;
    g->charge_complete    = false;
    g->full_hold_ticks    = 0U;
  } else if (!g->prev_vbus && vbus_present) {
    g->unplug_grace_ticks = 0U;
    g->trend_ticks        = 0U;
    g->charge_complete    = false;
    g->full_hold_ticks    = 0U;
  }
  g->prev_vbus = vbus_present;

  if (vbus_present) {
    update_charge_complete(g, vbat_adc, inst, hw_charging);
    bool eff_chg     = hw_charging && !g->charge_complete;
    g->prev_charging = eff_chg;

    uint8_t target = eff_chg ? ((inst > 99U) ? 99U : inst) : ((inst >= 92U) ? 100U : inst);
    if (eff_chg && g->pct > 99U) {
      g->pct         = 99U;
      g->trend_ticks = 0U;
    }
    if (g->charge_complete && target == 100U && g->pct == 99U && hw_charging) {
      /* Already confirmed TK75_BATT_FULL_CONFIRM_TICKS at CV-full voltage; promote cleanly now */
      g->pct         = 100U;
      g->trend_ticks = 0U;
      return g->pct;
    }
    if (target > g->pct) {
      g->trend_ticks++;
      if (g->trend_ticks >= TK75_BATT_HYST_CONFIRM_TICKS) {
        g->pct++;
        g->trend_ticks = 0U;
      }
    } else {
      g->trend_ticks = 0U;
    }
    return g->pct;
  }

  g->prev_charging = false;

  if (edge_unplug) {
    return g->pct;
  }

  if (g->unplug_grace_ticks > 0U) {
    g->unplug_grace_ticks--;
    g->trend_ticks = 0U;
  } else if (inst < g->pct) {
    g->trend_ticks++;
    if (g->trend_ticks >= TK75_BATT_HYST_CONFIRM_TICKS) {
      if (g->pct > 1U) {
        g->pct--;
      }
      g->trend_ticks = 0U;
    }
  } else {
    g->trend_ticks = 0U;
  }

  return g->pct;
}
