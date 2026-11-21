#ifndef KEYMAP_H
#define KEYMAP_H

#include "default_keymap_iso.h"
#include "flash_eeprom.h"
#include "hall_engine.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint8_t  modifiers;
  uint8_t  nkro_bitmap[16];
  uint8_t  boot_keys[6];
  uint16_t consumer_usage;
  uint8_t  system_usage;
  uint8_t  active_layer;
  uint16_t pressed_count;
} keymap_report_t;

/** Action callback invoked for quantum/RGB/Hall/TFT/Bootloader keycodes on edge press. */
typedef void (*keymap_action_cb_t)(uint16_t keycode, bool pressed, void *ctx);

typedef struct {
  tk75_persist_cfg_t *cfg;
  hall_engine_t      *hall;
  bool                mac_mode;
  uint32_t            layer_state;   /* bitmask of toggled/active non-momentary layers */
  uint8_t             prev_pressed[TK75_KEYMAP_ROWS * HALL_COLS];
  uint16_t            active_codes[TK75_KEYMAP_ROWS * HALL_COLS]; /* Latched keycode at press */
  int8_t              enc_pulse_dir; /* Pending +/-1 virtual pulse for col 15 */
  uint8_t             enc_pulse_ticks;
  bool                enc_prev_btn;
  uint16_t            pulse_consumer; /* Edge-triggered consumer pulse from encoder */
  keymap_action_cb_t  action_cb;
  void               *action_ctx;
} keymap_engine_t;

/** Initialize keymap engine with config storage, Hall engine, and quantum callback. */
void keymap_engine_init(keymap_engine_t *km,
                        tk75_persist_cfg_t *cfg,
                        hall_engine_t *hall);

/** Register callback for quantum/RGB/TFT/Hall/bootloader actions. */
void keymap_set_action_cb(keymap_engine_t *km, keymap_action_cb_t cb, void *ctx);

/** Synchronize Mac/Win base layer preference from physical mode switch. */
void keymap_set_mac_mode(keymap_engine_t *km, bool is_mac);

/** Inject encoder events (+1/-1 clicks & push button) into virtual column 15 or actions. */
void keymap_push_encoder(keymap_engine_t *km, int8_t delta, bool btn_pressed);

/** Trigger overlay-layer (L1..L3) special action bound to encoder button (2,15); true if handled. */
bool keymap_try_encoder_overlay_action(keymap_engine_t *km);

/** Clear latched keymap encoder button/pulse state when encoder is routed away from keymap. */
void keymap_reset_encoder_state(keymap_engine_t *km);

/** Resolve current matrix state into HID Boot/NKRO/Consumer/System report. */
void keymap_build_report(keymap_engine_t *km, keymap_report_t *out);

/** Lookup resolved keycode on highest active layer (following KC_TRNS down to Layer 0). */
uint16_t keymap_lookup(tk75_persist_cfg_t const *cfg, uint32_t active_mask, uint8_t row, uint8_t col);

#ifdef __cplusplus
}
#endif

#endif /* KEYMAP_H */
