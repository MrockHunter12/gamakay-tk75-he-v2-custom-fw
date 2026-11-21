#ifndef MODE_SWITCHES_H
#define MODE_SWITCHES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  TK75_CONN_USB = 0,
  TK75_CONN_24G = 1,
  TK75_CONN_BT  = 2,
} tk75_conn_mode_t;

#define MODE_SWITCH_DEBOUNCE_TICKS 15U

typedef struct {
  uint8_t conn_mode;    /* tk75_conn_mode_t: 0=USB Wired, 1=2.4GHz, 2=Bluetooth */
  bool    vbus_present; /* PC13 == 0: USB/charger cable plugged in */
  bool    charging;     /* PC13 == 0 && PC14 != 0: battery actively charging */
} mode_switch_state_t;

typedef struct {
  mode_switch_state_t stable;
  mode_switch_state_t candidate;
  uint8_t             count;
  bool                initialized;
} mode_switch_debouncer_t;

/** Pure decoder for verified hardware GPIO levels (host-testable). */
mode_switch_state_t mode_switches_decode(bool pb2_high, bool pb10_high, bool pc13_high, bool pc14_high);

/** Step consecutive-sample debouncer for slider/VBUS/charging transitions (host-testable). */
mode_switch_state_t mode_switches_debounce_step(mode_switch_debouncer_t *db, mode_switch_state_t raw);

/** Initialize GPIOB/GPIOC input pull-ups for 3-way slider (PB2/PB10), VBUS (PC13), charger STAT (PC14). */
void mode_switches_init(void);

/** Sample and debounce/decode physical slider, VBUS, and charger status. */
mode_switch_state_t mode_switches_read(void);

/** Convert filtered VBAT 12-bit ADC value into raw battery percentage (1..100). */
uint8_t tk75_battery_adc_to_pct(uint16_t adc);

#define TK75_BATT_HYST_CONFIRM_TICKS 6U
#define TK75_BATT_UNPLUG_GRACE_TICKS 5U
#define TK75_BATT_FULL_CONFIRM_TICKS 8U
#define TK75_BATT_CV_FULL_ADC        1435U /* ~4.205V load-compensated cell voltage = full */
#define TK75_BATT_CV_DROP_ADC        1405U /* Hysteresis floor before leaving full state */

typedef struct {
  bool     tft_on;
  uint8_t  tft_brightness_pct;
  bool     rgb_rail_on;
  uint32_t rgb_channel_sum; /* Sum of all 90 LEDs' R+G+B bytes (0..68850) */
} tk75_vbat_load_t;

typedef struct {
  uint8_t pct;
  uint8_t trend_ticks;
  uint8_t unplug_grace_ticks;
  uint8_t full_hold_ticks;
  bool    charge_complete;
  bool    initialized;
  bool    prev_vbus;
  bool    prev_charging;
} tk75_battery_gauge_t;

/** Compensate VBAT ADC reading for active RGB/TFT rail IR drop (calibrated on hardware). */
uint16_t tk75_battery_compensate_vbat(uint16_t raw_vbat_adc, tk75_vbat_load_t const *load);

void    tk75_battery_gauge_init(tk75_battery_gauge_t *g);
uint8_t tk75_battery_gauge_step(tk75_battery_gauge_t *g, uint16_t vbat_adc, bool vbus_present, bool charging);

/** Returns true while USB-powered and actively charging (clears once full charge is confirmed). */
bool    tk75_battery_gauge_is_charging(tk75_battery_gauge_t const *g);

#ifdef __cplusplus
}
#endif

#endif /* MODE_SWITCHES_H */
