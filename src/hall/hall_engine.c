#include "hall_engine.h"
#include <string.h>

#define HALL_FILTER_SHIFT        2U    /* EMA alpha = 1/4 for noise smoothing */
#define HALL_IDLE_REZERO_TICKS   1500U /* ~1.1 s at 1.33 kHz full-matrix scan rate */
#define HALL_IDLE_STEP_TICKS     30U   /* ~22 ms per 1-count nudge once re-zeroing */
#define HALL_AUTO_ZERO_DEADBAND  160U  /* Residual per-key thermal drift window */
#define HALL_IDLE_DEADZONE_UM    60U   /* 0.06 mm bottom noise floor (below 0.10 mm min actuation) */
#define HALL_MIN_SPAN_ADC        200U
#define HALL_RT_CONTINUOUS_FLOOR 50U

/* Common-mode ratiometric rail tracking (Q12 fixed point: 4096 = 1.000x).
 * Eliminates simultaneous resting-key drop when unplugging USB or changing RGB load. */
#define HALL_RAIL_SCALE_ONE_Q12  4096U
#define HALL_RAIL_SCALE_MIN_Q12  3072U /* 0.75x floor */
#define HALL_RAIL_SCALE_MAX_Q12  4710U /* 1.15x ceiling */
#define HALL_RAIL_TOP_K          24U   /* Track upper 24 ratios (immune to <=58 pressed keys) */
#define HALL_RAIL_AVG_START      8U    /* Skip top 8 to reject upward outliers */
#define HALL_RAIL_AVG_COUNT      16U   /* Average ranks 8..23 for low-noise rail estimate */
#define HALL_RAIL_MIN_ACTIVE     32U   /* Require full-keyboard scan before adjusting rail */
#define HALL_RAIL_SNAP_DELTA     16U   /* >=0.4% step snaps immediately on same frame */

/* Populated switch slots verified from tk75hev2_iso_v506_plain (row-major 6x15) */
static uint8_t const k_tk75_active_slots[HALL_SLOT_COUNT] = {
  /* R0 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
  /* R1 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  /* R2 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,0,1,
  /* R3 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,
  /* R4 */ 1,1,1,1,1,1,1,1,1,1,1,1,1,1,0,
  /* R5 */ 1,1,1,0,0,0,1,0,0,1,1,1,1,1,1,
};

static uint32_t slot_index(uint8_t row, uint8_t col)
{
  return ((uint32_t)row * HALL_COLS) + col;
}

static uint16_t clamp_u16(int32_t val, uint16_t lo, uint16_t hi)
{
  if (val < (int32_t)lo) {
    return lo;
  }
  if (val > (int32_t)hi) {
    return hi;
  }
  return (uint16_t)val;
}

static void update_filtered_adc(hall_key_state_t *st, uint16_t raw_adc)
{
  if (st->filtered_adc == 0U) {
    st->filtered_adc = raw_adc;
    return;
  }
  int32_t diff     = (int32_t)raw_adc - (int32_t)st->filtered_adc;
  int32_t abs_diff = (diff >= 0) ? diff : -diff;
  /* Adaptive EMA: fast (1/4) on real keypresses/steps, smooth (1/8) on small idle ripple */
  uint8_t shift = (abs_diff < 40) ? 3U : HALL_FILTER_SHIFT;
  int32_t half  = 1 << (shift - 1U);
  int32_t step  = (diff >= 0) ? ((diff + half) >> shift)
                              : -(((-diff) + half) >> shift);
  st->filtered_adc = (uint16_t)((int32_t)st->filtered_adc + step);
}

/* Estimate instantaneous Hall VCC ratio across resting keys (immune to active keypresses). */
static void update_common_mode_rail(hall_engine_t *engine)
{
  uint16_t top[HALL_RAIL_TOP_K];
  memset(top, 0, sizeof(top));
  uint8_t valid_count = 0U;

  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (engine->active_mask[i] == 0U) {
      continue;
    }
    hall_key_state_t const *st = &engine->states[i];
    if (st->rest_adc < 1000U || st->filtered_adc < 1000U || st->filtered_adc > 4050U) {
      continue;
    }
    valid_count++;
    uint32_t ratio = (((uint32_t)st->filtered_adc << 12U) + (st->rest_adc >> 1U)) / st->rest_adc;
    if (ratio > 65535U) {
      ratio = 65535U;
    }
    uint16_t r16 = (uint16_t)ratio;
    if (r16 <= top[HALL_RAIL_TOP_K - 1U]) {
      continue;
    }
    uint8_t pos = HALL_RAIL_TOP_K - 1U;
    while (pos > 0U && r16 > top[pos - 1U]) {
      top[pos] = top[pos - 1U];
      pos--;
    }
    top[pos] = r16;
  }

  if (valid_count < HALL_RAIL_MIN_ACTIVE) {
    return;
  }

  uint32_t sum = 0U;
  for (uint8_t i = HALL_RAIL_AVG_START; i < (HALL_RAIL_AVG_START + HALL_RAIL_AVG_COUNT); i++) {
    sum += top[i];
  }
  uint16_t target = clamp_u16((int32_t)((sum + (HALL_RAIL_AVG_COUNT / 2U)) / HALL_RAIL_AVG_COUNT),
                              HALL_RAIL_SCALE_MIN_Q12, HALL_RAIL_SCALE_MAX_Q12);
  int32_t  diff     = (int32_t)target - (int32_t)engine->rail_scale_q12;
  int32_t  abs_diff = (diff >= 0) ? diff : -diff;
  if (abs_diff >= (int32_t)HALL_RAIL_SNAP_DELTA) {
    engine->rail_scale_q12 = target;
  } else if (diff != 0) {
    int32_t step = (diff > 0) ? ((diff + 1) >> 1U) : -(((-diff) + 1) >> 1U);
    engine->rail_scale_q12 = (uint16_t)((int32_t)engine->rail_scale_q12 + step);
  }
}

static uint16_t compute_travel_um(hall_key_state_t *st, uint16_t rail_scale_q12)
{
  uint16_t eff_rest = (uint16_t)(((uint32_t)st->rest_adc * rail_scale_q12 + 2048U) >> 12U);
  uint16_t eff_span = (uint16_t)(((uint32_t)st->span_adc * rail_scale_q12 + 2048U) >> 12U);
  if (eff_span < HALL_MIN_SPAN_ADC) {
    eff_span = HALL_MIN_SPAN_ADC;
  }

  /* Gentle bidirectional per-key idle baseline tracking for residual thermal drift */
  int32_t drift     = (int32_t)st->filtered_adc - (int32_t)eff_rest;
  int32_t abs_drift = (drift >= 0) ? drift : -drift;
  if (!st->raw_pressed && abs_drift > 0 && abs_drift <= (int32_t)HALL_AUTO_ZERO_DEADBAND) {
    st->idle_ticks++;
    if (st->idle_ticks >= HALL_IDLE_REZERO_TICKS) {
      st->rest_adc   = (drift > 0) ? (uint16_t)(st->rest_adc + 1U) : (uint16_t)(st->rest_adc - 1U);
      st->idle_ticks = (uint16_t)(HALL_IDLE_REZERO_TICKS - HALL_IDLE_STEP_TICKS);
      eff_rest       = (uint16_t)(((uint32_t)st->rest_adc * rail_scale_q12 + 2048U) >> 12U);
    }
  } else {
    st->idle_ticks = 0U;
  }

  if (st->filtered_adc >= eff_rest) {
    return 0U;
  }

  uint32_t delta  = (uint32_t)(eff_rest - st->filtered_adc);
  uint32_t travel = (delta * HALL_MAX_TRAVEL_UM) / eff_span;
  if (travel < HALL_IDLE_DEADZONE_UM) {
    return 0U;
  }
  return (travel > HALL_MAX_TRAVEL_UM) ? HALL_MAX_TRAVEL_UM : (uint16_t)travel;
}

static void update_fixed_trigger(hall_key_config_t const *cfg, hall_key_state_t *st)
{
  uint16_t release_thr = (cfg->actuation_um > cfg->release_hyst_um)
                             ? (uint16_t)(cfg->actuation_um - cfg->release_hyst_um)
                             : 50U;
  if (!st->raw_pressed && st->travel_um >= cfg->actuation_um) {
    st->raw_pressed = true;
  } else if (st->raw_pressed && st->travel_um <= release_thr) {
    st->raw_pressed = false;
  }
}

static void update_rapid_trigger(hall_key_config_t const *cfg, hall_key_state_t *st)
{
  uint16_t floor_um = cfg->rt_continuous ? HALL_RT_CONTINUOUS_FLOOR : cfg->actuation_um;

  /* Latch RT activation if RT was enabled dynamically while switch was already pressed */
  if (st->raw_pressed) {
    st->rt_armed = true;
  }

  if (!st->rt_armed) {
    if (st->travel_um >= cfg->actuation_um) {
      st->rt_armed    = true;
      st->raw_pressed = true;
      st->extremum_um = st->travel_um;
    }
    return;
  }

  if (st->travel_um < floor_um) {
    st->raw_pressed = false;
    st->rt_armed    = false;
    st->extremum_um = st->travel_um;
    return;
  }

  if (!st->raw_pressed) {
    if (st->travel_um < st->extremum_um) {
      st->extremum_um = st->travel_um;
    }
    if ((uint32_t)st->travel_um >= (uint32_t)st->extremum_um + cfg->rt_down_um) {
      st->raw_pressed = true;
      st->extremum_um = st->travel_um;
    }
    return;
  }

  if (st->travel_um > st->extremum_um) {
    st->extremum_um = st->travel_um;
  }
  if ((uint32_t)st->travel_um + cfg->rt_up_um <= (uint32_t)st->extremum_um) {
    st->raw_pressed = false;
    st->extremum_um = st->travel_um;
  }
}

static bool valid_pos(hall_pos_t pos)
{
  return (pos.row < HALL_ROWS) && (pos.col < HALL_COLS);
}

static void resolve_single_socd_pair(hall_engine_t *engine, uint8_t idx)
{
  hall_socd_pair_t const *pair = &engine->socd_pairs[idx];
  hall_socd_runtime_t *rt = &engine->socd_rt[idx];

  if (pair->mode == HALL_SOCD_OFF || !valid_pos(pair->key_a) || !valid_pos(pair->key_b)) {
    return;
  }

  uint32_t slot_a = slot_index(pair->key_a.row, pair->key_a.col);
  uint32_t slot_b = slot_index(pair->key_b.row, pair->key_b.col);
  bool a = engine->states[slot_a].raw_pressed;
  bool b = engine->states[slot_b].raw_pressed;

  if (a && !rt->active_a) {
    rt->last_winner = 1U;
    if (!b) {
      rt->first_winner = 1U;
    }
  }
  if (b && !rt->active_b) {
    rt->last_winner = 2U;
    if (!a) {
      rt->first_winner = 2U;
    }
  }
  rt->active_a = a;
  rt->active_b = b;

  if (!a || !b) {
    if (a && !b) {
      rt->last_winner  = 1U;
      rt->first_winner = 1U;
    } else if (b && !a) {
      rt->last_winner  = 2U;
      rt->first_winner = 2U;
    } else {
      rt->last_winner  = 0U;
      rt->first_winner = 0U;
    }
    return;
  }

  /* Both keys physically pressed simultaneously */
  if (pair->mode == HALL_SOCD_NEUTRAL) {
    engine->states[slot_a].logical_pressed = false;
    engine->states[slot_b].logical_pressed = false;
  } else if (pair->mode == HALL_SOCD_LAST_PRIORITY) {
    engine->states[slot_a].logical_pressed = (rt->last_winner == 1U);
    engine->states[slot_b].logical_pressed = (rt->last_winner == 2U);
  } else if (pair->mode == HALL_SOCD_FIRST_PRIORITY) {
    engine->states[slot_a].logical_pressed = (rt->first_winner == 1U);
    engine->states[slot_b].logical_pressed = (rt->first_winner == 2U);
  }
}

void hall_engine_init(hall_engine_t *engine, uint16_t const rest_adc[HALL_SLOT_COUNT], uint16_t const span_adc[HALL_SLOT_COUNT])
{
  memset(engine, 0, sizeof(*engine));
  memcpy(engine->active_mask, k_tk75_active_slots, sizeof(k_tk75_active_slots));
  engine->rail_scale_q12 = HALL_RAIL_SCALE_ONE_Q12;

  hall_key_config_t def_cfg = {
    .actuation_um    = HALL_DEFAULT_ACT_UM,
    .release_hyst_um = HALL_DEFAULT_HYST_UM,
    .rt_enabled      = false,
    .rt_continuous   = false,
    .rt_down_um      = HALL_DEFAULT_RT_DOWN_UM,
    .rt_up_um        = HALL_DEFAULT_RT_UP_UM,
  };

  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    engine->configs[i] = def_cfg;
    engine->states[i].rest_adc = (rest_adc != NULL && rest_adc[i] > 800U) ? rest_adc[i] : HALL_DEFAULT_REST_ADC;
    engine->states[i].span_adc = (span_adc != NULL && span_adc[i] >= HALL_MIN_SPAN_ADC) ? span_adc[i] : HALL_DEFAULT_SPAN_ADC;
    engine->states[i].cal_valid = (rest_adc != NULL);
  }
}

void hall_engine_set_global_config(hall_engine_t *engine, hall_key_config_t const *cfg)
{
  hall_key_config_t clean = *cfg;
  clean.actuation_um    = clamp_u16(clean.actuation_um, 100U, 3900U);
  clean.release_hyst_um = clamp_u16(clean.release_hyst_um, 50U, 1000U);
  clean.rt_down_um      = clamp_u16(clean.rt_down_um, 100U, 2000U);
  clean.rt_up_um        = clamp_u16(clean.rt_up_um, 100U, 2000U);

  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    engine->configs[i] = clean;
  }
}

void hall_engine_set_key_config(hall_engine_t *engine, hall_pos_t pos, hall_key_config_t const *cfg)
{
  if (!valid_pos(pos)) {
    return;
  }
  hall_key_config_t clean = *cfg;
  clean.actuation_um    = clamp_u16(clean.actuation_um, 100U, 3900U);
  clean.release_hyst_um = clamp_u16(clean.release_hyst_um, 50U, 1000U);
  clean.rt_down_um      = clamp_u16(clean.rt_down_um, 100U, 2000U);
  clean.rt_up_um        = clamp_u16(clean.rt_up_um, 100U, 2000U);
  engine->configs[slot_index(pos.row, pos.col)] = clean;
}

void hall_engine_set_socd_pair(hall_engine_t *engine, uint8_t pair_idx, hall_socd_pair_t const *pair)
{
  if (pair_idx >= HALL_SOCD_MAX_PAIRS) {
    return;
  }
  engine->socd_pairs[pair_idx] = *pair;
  memset(&engine->socd_rt[pair_idx], 0, sizeof(hall_socd_runtime_t));
}

void hall_engine_seed_idle_baseline(hall_engine_t *engine, uint16_t const raw_frame[HALL_SLOT_COUNT])
{
  engine->rail_scale_q12 = HALL_RAIL_SCALE_ONE_Q12;
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (engine->active_mask[i] != 0U && raw_frame[i] > 1200U && raw_frame[i] < 4050U) {
      engine->states[i].rest_adc     = raw_frame[i];
      engine->states[i].filtered_adc = raw_frame[i];
      engine->states[i].cal_valid    = true;
    }
  }
}

void hall_engine_process_frame(hall_engine_t *engine, uint16_t const raw_frame[HALL_SLOT_COUNT])
{
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (engine->active_mask[i] == 0U) {
      engine->states[i].raw_pressed     = false;
      engine->states[i].logical_pressed = false;
      continue;
    }
    update_filtered_adc(&engine->states[i], raw_frame[i]);
  }

  update_common_mode_rail(engine);

  uint16_t max_travel = 0U;
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (engine->active_mask[i] == 0U) {
      continue;
    }
    hall_key_state_t        *st  = &engine->states[i];
    hall_key_config_t const *cfg = &engine->configs[i];

    st->travel_um = compute_travel_um(st, engine->rail_scale_q12);
    if (st->travel_um > max_travel) {
      max_travel = st->travel_um;
    }
    if (cfg->rt_enabled) {
      update_rapid_trigger(cfg, st);
    } else {
      update_fixed_trigger(cfg, st);
    }
    st->logical_pressed = st->raw_pressed;
  }

  hall_engine_apply_socd(engine);

  uint16_t pressed = 0U;
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (engine->active_mask[i] != 0U && engine->states[i].logical_pressed) {
      pressed++;
    }
  }
  engine->max_travel_um = max_travel;
  engine->pressed_count = pressed;
  engine->any_actuated  = (pressed > 0U);
}

void hall_engine_apply_socd(hall_engine_t *engine)
{
  for (uint8_t i = 0U; i < HALL_SOCD_MAX_PAIRS; i++) {
    resolve_single_socd_pair(engine, i);
  }
}

bool hall_engine_is_pressed(hall_engine_t const *engine, uint8_t row, uint8_t col)
{
  if (row >= HALL_ROWS || col >= HALL_COLS) {
    return false;
  }
  return engine->states[slot_index(row, col)].logical_pressed;
}

uint16_t hall_engine_get_travel_um(hall_engine_t const *engine, uint8_t row, uint8_t col)
{
  if (row >= HALL_ROWS || col >= HALL_COLS) {
    return 0U;
  }
  return engine->states[slot_index(row, col)].travel_um;
}
