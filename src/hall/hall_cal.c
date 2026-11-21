#include "hall_cal.h"
#include <string.h>

static void copy_label(char out[5], char const *src)
{
  size_t i = 0U;
  while (i < 4U && src[i] != '\0') {
    out[i] = src[i];
    i++;
  }
  out[i] = '\0';
}

void hall_cal_format_key_label(uint16_t kc, char out[5])
{
  if (kc >= KC_A && kc <= KC_Z) {
    out[0] = (char)('A' + (kc - KC_A));
    out[1] = '\0';
    return;
  }
  if (kc >= KC_1 && kc <= KC_9) {
    out[0] = (char)('1' + (kc - KC_1));
    out[1] = '\0';
    return;
  }
  if (kc == KC_0) {
    copy_label(out, "0");
    return;
  }
  if (kc >= KC_F1 && kc <= KC_F12) {
    uint8_t n = (uint8_t)(1U + (kc - KC_F1));
    out[0] = 'F';
    if (n >= 10U) {
      out[1] = '1';
      out[2] = (char)('0' + (n - 10U));
      out[3] = '\0';
    } else {
      out[1] = (char)('0' + n);
      out[2] = '\0';
    }
    return;
  }

  switch (kc) {
    case KC_ESC:  copy_label(out, "ESC");  return;
    case KC_TAB:  copy_label(out, "TAB");  return;
    case KC_CAPS: copy_label(out, "CAP");  return;
    case KC_LSFT: copy_label(out, "LSFT"); return;
    case KC_RSFT: copy_label(out, "RSFT"); return;
    case KC_LCTL: copy_label(out, "LCTL"); return;
    case KC_RCTL: copy_label(out, "RCTL"); return;
    case KC_LGUI: copy_label(out, "WIN");  return;
    case KC_RGUI: copy_label(out, "RGUI"); return;
    case KC_LALT: copy_label(out, "ALT");  return;
    case KC_RALT: copy_label(out, "ALTG"); return;
    case KC_SPC:  copy_label(out, "SPC");  return;
    case KC_ENT:  copy_label(out, "ENT");  return;
    case KC_BSPC: copy_label(out, "BSP");  return;
    case KC_DEL:  copy_label(out, "DEL");  return;
    case KC_HOME: copy_label(out, "HOM");  return;
    case KC_END:  copy_label(out, "END");  return;
    case KC_PGUP: copy_label(out, "PGU");  return;
    case KC_PGDN: copy_label(out, "PGD");  return;
    case KC_UP:   copy_label(out, "UP");   return;
    case KC_DOWN: copy_label(out, "DN");   return;
    case KC_LEFT: copy_label(out, "LFT");  return;
    case KC_RGHT: copy_label(out, "RGT");  return;
    case KC_GRV:  copy_label(out, "`");    return;
    case KC_MINS: copy_label(out, "-");    return;
    case KC_EQL:  copy_label(out, "=");    return;
    case KC_LBRC: copy_label(out, "[");    return;
    case KC_RBRC: copy_label(out, "]");    return;
    case KC_SCLN: copy_label(out, ";");    return;
    case KC_QUOT: copy_label(out, "'");    return;
    case KC_NUHS: copy_label(out, "#");    return;
    case KC_NUBS: copy_label(out, "<>");   return;
    case KC_COMM: copy_label(out, ",");    return;
    case KC_DOT:  copy_label(out, ".");    return;
    case KC_SLSH: copy_label(out, "/");    return;
    default:
      if ((kc & 0xFFE0U) == QK_MO_BASE) {
        copy_label(out, "FN");
        return;
      }
      copy_label(out, "KEY");
      return;
  }
}

void hall_cal_init(hall_cal_t *cal)
{
  if (cal == NULL) {
    return;
  }
  memset(cal, 0, sizeof(*cal));
  cal->active_slot  = 0xFFU;
  cal->pressed_slot = 0xFFU;
  copy_label(cal->active_label, "--");
}

void hall_cal_start(hall_cal_t *cal, hall_engine_t const *hall)
{
  if (cal == NULL || hall == NULL) {
    return;
  }
  memset(cal, 0, sizeof(*cal));
  cal->active             = true;
  cal->wait_start_release = true;
  cal->active_slot        = 0xFFU;
  cal->pressed_slot       = 0xFFU;
  copy_label(cal->active_label, "PUSH");

  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (hall->active_mask[i] == 0U) {
      continue;
    }
    uint16_t base = (hall->states[i].filtered_adc > 800U)
                      ? hall->states[i].filtered_adc
                      : hall->states[i].rest_adc;
    cal->rest_adc[i] = base;
    cal->min_adc[i]  = base;
    cal->total_keys++;
  }
}

void hall_cal_cancel(hall_cal_t *cal)
{
  if (cal != NULL) {
    cal->active = false;
  }
}

bool hall_cal_is_active(hall_cal_t const *cal)
{
  return (cal != NULL) && cal->active;
}

static void commit_calibration(hall_cal_t const *cal, hall_engine_t *hall, tk75_persist_cfg_t *cfg)
{
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (hall->active_mask[i] == 0U || cal->done_mask[i] == 0U) {
      continue;
    }
    uint32_t raw_span = (cal->rest_adc[i] > cal->min_adc[i])
                          ? (uint32_t)(cal->rest_adc[i] - cal->min_adc[i])
                          : 0U;
    uint32_t span = (raw_span * HALL_CAL_MARGIN_NUM) / HALL_CAL_MARGIN_DEN;
    if (span < 250U) {
      span = 250U;
    }
    if (span > 2400U) {
      span = 2400U;
    }
    hall->states[i].rest_adc  = cal->rest_adc[i];
    hall->states[i].span_adc  = (uint16_t)span;
    hall->states[i].cal_valid = true;
    if (cfg != NULL) {
      cfg->hall_user_span_adc[i] = (uint16_t)span;
    }
  }
}

hall_cal_evt_t hall_cal_step(hall_cal_t *cal, hall_engine_t *hall, tk75_persist_cfg_t *cfg)
{
  if (cal == NULL || !cal->active || hall == NULL || cfg == NULL) {
    return HALL_CAL_EVT_NONE;
  }

  /* Wait for all keys to be released after entering calibration from menu */
  if (cal->wait_start_release) {
    bool any_down = false;
    for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
      if (hall->active_mask[i] == 0U) {
        continue;
      }
      uint16_t cur = hall->states[i].filtered_adc;
      if (cal->rest_adc[i] > cur && (uint16_t)(cal->rest_adc[i] - cur) >= HALL_CAL_RELEASE_DELTA) {
        any_down = true;
      }
    }
    if (!any_down) {
      cal->wait_start_release = false;
    }
    return HALL_CAL_EVT_NONE;
  }

  uint8_t  deepest_slot  = 0xFFU;
  uint16_t deepest_delta = 0U;
  bool     any_still_down = false;

  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (hall->active_mask[i] == 0U) {
      continue;
    }
    uint16_t cur   = hall->states[i].filtered_adc;
    uint16_t delta = (cal->rest_adc[i] > cur) ? (uint16_t)(cal->rest_adc[i] - cur) : 0U;

    /* Track true deepest bottom-out ADC */
    if (cur < cal->min_adc[i]) {
      cal->min_adc[i] = cur;
    }
    if (delta >= HALL_CAL_RELEASE_DELTA) {
      any_still_down = true;
    }
    /* Mark key calibrated as soon as it crosses valid full-stroke threshold (still tracks deeper!) */
    if (cal->done_mask[i] == 0U &&
        cal->rest_adc[i] > cal->min_adc[i] &&
        (uint16_t)(cal->rest_adc[i] - cal->min_adc[i]) >= HALL_CAL_MIN_VALID_DELTA) {
      cal->done_mask[i] = 1U;
      cal->done_count++;
    }

    if (delta > deepest_delta && delta >= HALL_CAL_RELEASE_DELTA) {
      deepest_delta = delta;
      deepest_slot  = (uint8_t)i;
    }
  }

  if (deepest_slot != 0xFFU) {
    uint8_t  r   = deepest_slot / HALL_COLS;
    uint8_t  c   = deepest_slot % HALL_COLS;
    uint16_t ref = (hall->states[deepest_slot].span_adc >= 400U)
                     ? hall->states[deepest_slot].span_adc
                     : 1000U;
    uint32_t pct = ((uint32_t)deepest_delta * 100U) / ref;
    if (pct > 100U) {
      pct = 100U;
    }
    cal->active_slot  = deepest_slot;
    cal->pressed_slot = deepest_slot;
    cal->active_delta = deepest_delta;
    cal->strength_pct = (uint8_t)pct;
    cal->active_done  = (cal->done_mask[deepest_slot] != 0U);
    hall_cal_format_key_label(cfg->keymap[0][r][c], cal->active_label);
  } else {
    cal->pressed_slot = 0xFFU;
    cal->strength_pct = 0U;
    cal->active_delta = 0U;
    if (cal->active_slot != 0xFFU) {
      cal->active_done = (cal->done_mask[cal->active_slot] != 0U);
    }
  }

  /* Auto-finish once ALL 82 active keys have been pressed deep and released cleanly */
  if (cal->total_keys > 0U && cal->done_count >= cal->total_keys && !any_still_down) {
    commit_calibration(cal, hall, cfg);
    cal->active = false;
    return HALL_CAL_EVT_FINISHED;
  }
  return HALL_CAL_EVT_NONE;
}
