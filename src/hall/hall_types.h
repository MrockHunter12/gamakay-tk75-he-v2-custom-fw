#ifndef HALL_TYPES_H
#define HALL_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HALL_ROWS                6U
#define HALL_COLS                15U
#define HALL_SLOT_COUNT          (HALL_ROWS * HALL_COLS)

#define HALL_MAX_TRAVEL_UM       4000U
#define HALL_DEFAULT_REST_ADC    2400U
#define HALL_DEFAULT_SPAN_ADC    1100U
#define HALL_DEFAULT_ACT_UM      1800U
#define HALL_DEFAULT_HYST_UM     250U
#define HALL_DEFAULT_RT_DOWN_UM  350U
#define HALL_DEFAULT_RT_UP_UM    350U
#define HALL_SOCD_MAX_PAIRS      4U

typedef enum {
  HALL_SOCD_OFF = 0,
  HALL_SOCD_LAST_PRIORITY = 1,
  HALL_SOCD_NEUTRAL = 2,
  HALL_SOCD_FIRST_PRIORITY = 3
} hall_socd_mode_t;

typedef struct {
  uint8_t row;
  uint8_t col;
} hall_pos_t;

typedef struct {
  hall_pos_t       key_a;
  hall_pos_t       key_b;
  hall_socd_mode_t mode;
} hall_socd_pair_t;

typedef struct {
  uint16_t actuation_um;    /* 100..3900 um */
  uint16_t release_hyst_um; /* 50..1000 um */
  bool     rt_enabled;
  bool     rt_continuous;   /* true: release works all the way above continuous floor */
  uint16_t rt_down_um;      /* 100..2000 um */
  uint16_t rt_up_um;        /* 100..2000 um */
} hall_key_config_t;

typedef struct {
  uint16_t rest_adc;        /* Zero-travel baseline ADC reading (higher voltage) */
  uint16_t span_adc;        /* Full-travel delta in ADC counts (rest - bottom) */
  uint16_t filtered_adc;
  uint16_t travel_um;       /* 0..4000 um */
  uint16_t extremum_um;     /* Local peak when pressed / trough when released for RT */
  uint16_t idle_ticks;
  bool     cal_valid;
  bool     raw_pressed;
  bool     logical_pressed;
  bool     rt_armed;
} hall_key_state_t;

typedef struct {
  bool     active_a;
  bool     active_b;
  uint8_t  last_winner;     /* 0=none, 1=A, 2=B */
  uint8_t  first_winner;
} hall_socd_runtime_t;

typedef struct {
  hall_key_config_t   configs[HALL_SLOT_COUNT];
  hall_key_state_t    states[HALL_SLOT_COUNT];
  hall_socd_pair_t    socd_pairs[HALL_SOCD_MAX_PAIRS];
  hall_socd_runtime_t socd_rt[HALL_SOCD_MAX_PAIRS];
  uint8_t             active_mask[HALL_SLOT_COUNT]; /* 1 = populated switch, 0 = empty/VBAT */
  uint16_t            rail_scale_q12;               /* Common-mode Hall supply rail ratio (4096 = 1.0x) */
} hall_engine_t;

#ifdef __cplusplus
}
#endif

#endif /* HALL_TYPES_H */
