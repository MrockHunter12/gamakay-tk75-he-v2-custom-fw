#ifndef HALL_CAL_H
#define HALL_CAL_H

#include "default_keymap_iso.h"
#include "flash_eeprom.h"
#include "hall_engine.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALL_CAL_MIN_VALID_DELTA 320U /* Minimum ADC drop to accept as a full bottom-out */
#define HALL_CAL_RELEASE_DELTA   80U  /* ADC drop below which key is considered released */
#define HALL_CAL_MARGIN_NUM      97U  /* 97% of peak delta so normal bottom-out reaches 4.0mm */
#define HALL_CAL_MARGIN_DEN      100U

typedef enum {
  HALL_CAL_EVT_NONE = 0,
  HALL_CAL_EVT_FINISHED = 1,
} hall_cal_evt_t;

typedef struct {
  bool     active;
  bool     wait_start_release;
  uint8_t  total_keys;
  uint8_t  done_count;
  uint8_t  active_slot;  /* Last pressed slot for TFT display (0..89, 0xFF if none) */
  uint8_t  pressed_slot; /* Currently held slot for live RGB highlight (0xFF if released) */
  uint8_t  strength_pct; /* Live press strength 0..100% of active_slot */
  uint16_t active_delta; /* Live ADC deflection of active_slot */
  bool     active_done;  /* True if active_slot has reached calibrated depth */
  char     active_label[5];
  uint16_t rest_adc[HALL_SLOT_COUNT];
  uint16_t min_adc[HALL_SLOT_COUNT];
  uint8_t  done_mask[HALL_SLOT_COUNT]; /* 0=pending, 1=calibrated (bottom reached) */
} hall_cal_t;

/** Initialize calibration controller to inactive state. */
void hall_cal_init(hall_cal_t *cal);

/** Start interactive 82-key bottom-out calibration from current live Hall rest baseline. */
void hall_cal_start(hall_cal_t *cal, hall_engine_t const *hall);

/** Cancel active calibration without altering saved calibration spans. */
void hall_cal_cancel(hall_cal_t *cal);

/** True while interactive Hall calibration owns keyboard input, RGB, and TFT. */
bool hall_cal_is_active(hall_cal_t const *cal);

/** Format a short 1..4 char keycap label for a Layer-0 QMK keycode (e.g. "A", "SPC", "F12"). */
void hall_cal_format_key_label(uint16_t kc, char out[5]);

/** Advance calibration from live Hall sensor state; commits spans & returns FINISHED once all keys done. */
hall_cal_evt_t hall_cal_step(hall_cal_t *cal, hall_engine_t *hall, tk75_persist_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* HALL_CAL_H */
