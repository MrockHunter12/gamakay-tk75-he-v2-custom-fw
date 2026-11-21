#ifndef RGB_EFFECTS_H
#define RGB_EFFECTS_H

#include "flash_eeprom.h"
#include "hall_engine.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RGB_LED_COUNT      90U
#define RGB_KEY_LED_COUNT  82U
#define RGB_SIDE_LED_COUNT 8U

typedef enum {
  RGB_FX_OFF = 0,
  RGB_FX_RAINBOW_WAVE = 1,
  RGB_FX_SOLID = 2,
  RGB_FX_BREATHING = 3,
  RGB_FX_HE_ANALOG_DEPTH = 4, /* Per-key brightness & hue shift live with magnet travel */
  RGB_FX_REACTIVE_HEAT = 5,   /* Bright flash decaying smoothly after key press */
  RGB_FX_LAYER_INDICATOR = 6, /* Highlights bound keys on the active layer */
} rgb_effect_id_t;

typedef enum {
  CAPS_SIDE_FX_SOLID = 0,
  CAPS_SIDE_FX_BREATHING = 1,
  CAPS_SIDE_FX_RAINBOW_WAVE = 2,
  CAPS_SIDE_FX_BLINK = 3,
  CAPS_SIDE_FX_SCANNER = 4,
  CAPS_SIDE_FX_COUNT = 5,
} caps_side_effect_id_t;

typedef struct {
  uint8_t g;
  uint8_t r;
  uint8_t b;
} rgb_grb_t;

typedef struct {
  uint8_t heatmap[RGB_KEY_LED_COUNT];
  uint8_t macro_glow[RGB_KEY_LED_COUNT];
} rgb_effects_state_t;

typedef struct {
  rgb_effects_state_t      *fx;
  tk75_persist_cfg_t const *cfg;
  hall_engine_t const      *hall;
  uint8_t                   active_layer;
  uint32_t                  layer_mask;
  uint8_t                   host_leds;
  bool                      macro_rec_active;
  uint8_t                   macro_rec_slot;
  bool                      macro_saved_flash;
  uint8_t                   macro_saved_slot;
  uint8_t const            *macro_typed_bitmap; /* Optional 32-byte HID bitmap of live macro typing */
  uint8_t const            *cal_done_mask;      /* Non-NULL during interactive Hall calibration */
  uint8_t                   cal_active_slot;    /* Currently pressed slot during Hall calibration (0xFF=none) */
  uint32_t                  now_ms;
} rgb_render_ctx_t;

/** Convert HSV (0..255 each) to 8-bit GRB color using fast integer math. */
rgb_grb_t rgb_hsv_to_grb(uint8_t hue, uint8_t sat, uint8_t val);

/** Lookup hardware serpentine LED index (0..81) for matrix (row, col), or 0xFF if unlit. */
uint8_t rgb_matrix_to_led_idx(uint8_t row, uint8_t col);

/** Render all 90 GRB LEDs cleanly from config, Hall analog state, active layer, and lock LEDs. */
void rgb_effects_render(rgb_render_ctx_t const *ctx, rgb_grb_t out_leds[RGB_LED_COUNT]);

#ifdef __cplusplus
}
#endif

#endif /* RGB_EFFECTS_H */
