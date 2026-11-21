#ifndef HALL_ENGINE_H
#define HALL_ENGINE_H

#include "hall_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize engine with default TK75 ISO active slots, actuation config, and SOCD defaults. */
void hall_engine_init(hall_engine_t *engine, uint16_t const rest_adc[HALL_SLOT_COUNT], uint16_t const span_adc[HALL_SLOT_COUNT]);

/** Apply one global actuation / Rapid Trigger preset to all populated keys. */
void hall_engine_set_global_config(hall_engine_t *engine, hall_key_config_t const *cfg);

/** Update per-key config for a single (row, col) position. */
void hall_engine_set_key_config(hall_engine_t *engine, hall_pos_t pos, hall_key_config_t const *cfg);

/** Configure one SOCD pair (index 0..HALL_SOCD_MAX_PAIRS-1). */
void hall_engine_set_socd_pair(hall_engine_t *engine, uint8_t pair_idx, hall_socd_pair_t const *pair);

/** Seed resting calibration from the first raw ADC frame captured at idle startup. */
void hall_engine_seed_idle_baseline(hall_engine_t *engine, uint16_t const raw_frame[HALL_SLOT_COUNT]);

/** Process one full 15x6 ADC sample frame and refresh raw_pressed & travel_um. */
void hall_engine_process_frame(hall_engine_t *engine, uint16_t const raw_frame[HALL_SLOT_COUNT]);

/** Evaluate all configured SOCD pairs and update logical_pressed outputs. */
void hall_engine_apply_socd(hall_engine_t *engine);

/** Query logical (post-SOCD) pressed state of a matrix slot. */
bool hall_engine_is_pressed(hall_engine_t const *engine, uint8_t row, uint8_t col);

/** Query travel distance in micrometers (0..4000) of a matrix slot. */
uint16_t hall_engine_get_travel_um(hall_engine_t const *engine, uint8_t row, uint8_t col);

#ifdef __cplusplus
}
#endif

#endif /* HALL_ENGINE_H */
