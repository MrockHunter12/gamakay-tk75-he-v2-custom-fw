#include "keymap.h"
#include <string.h>

/* Default 4-layer 6x16 ISO matrix matching verified tk75hev2_iso_v506_plain hardware slots */
static uint16_t const k_default_layers[TK75_KEYMAP_LAYERS][TK75_KEYMAP_ROWS][TK75_KEYMAP_COLS] = {
  /* Layer 0: Windows / Linux ISO 75% */
  {
    { KC_ESC,  KC_F1,   KC_F2,   KC_F3,   KC_F4,   KC_F5,   KC_F6,   KC_F7,   KC_F8,   KC_F9,   KC_F10,  KC_F11,   KC_F12,  KC_DEL,  KC_NO,   KC_VOLD },
    { KC_GRV,  KC_1,    KC_2,    KC_3,    KC_4,    KC_5,    KC_6,    KC_7,    KC_8,    KC_9,    KC_0,    KC_MINS,  KC_EQL,  KC_BSPC, KC_HOME, KC_VOLU },
    { KC_TAB,  KC_Q,    KC_W,    KC_E,    KC_R,    KC_T,    KC_Y,    KC_U,    KC_I,    KC_O,    KC_P,    KC_LBRC,  KC_RBRC, KC_NO,   KC_PGUP, KC_MUTE },
    { KC_CAPS, KC_A,    KC_S,    KC_D,    KC_F,    KC_G,    KC_H,    KC_J,    KC_K,    KC_L,    KC_SCLN, KC_QUOT,  KC_NUHS, KC_ENT,  KC_PGDN, KC_NO   },
    { KC_LSFT, KC_NUBS, KC_Z,    KC_X,    KC_C,    KC_V,    KC_B,    KC_N,    KC_M,    KC_COMM, KC_DOT,  KC_SLSH,  KC_RSFT, KC_UP,   KC_NO,   KC_NO   },
    { KC_LCTL, KC_LGUI, KC_LALT, KC_NO,   KC_NO,   KC_NO,   KC_SPC,  KC_NO,   KC_NO,   KC_RALT, MO(1),   KC_RCTL,  KC_LEFT, KC_DOWN, KC_RGHT, KC_NO   },
  },
  /* Layer 1: Fn Layer (RGB, TFT Page, SOCD, RT, Actuation Step, Media, Bootloader) */
  {
    { KC_GRV,      KC_BRID,  KC_BRIU,  KC_PSCR,  KC_TFT_TOG, KC_TFT_NEXT, KC_MPRV,   KC_MPLY,   KC_MNXT,  KC_MUTE,     KC_VOLD,  KC_VOLU,  KC_NO,    KC_NO,    KC_NO,   KC_TRNS },
    { KC_TRNS,     KC_RT_TOG,KC_SOCD_CYCLE,KC_ACT_DN,KC_ACT_UP,KC_TRNS,   KC_TRNS,   KC_TRNS,   KC_TRNS,  KC_TRNS,     KC_TRNS,  RGB_SPD,  RGB_SPI,  KC_TRNS,  KC_END,  KC_TRNS },
    { KC_TRNS,     KC_BT1,   KC_BT2,   KC_BT3,   KC_RF_24G,  KC_RF_PAIR,  KC_MAC_TOG,KC_TRNS,   KC_TRNS,  KC_TRNS,     KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_NO,    KC_TFT_PREV, KC_ENC_MODE_NEXT },
    { KC_TRNS,     KC_TRNS,  KC_TRNS,  KC_TRNS,  KC_TRNS,    KC_TRNS,     KC_TRNS,   KC_TRNS,   KC_TRNS,  KC_TRNS,     KC_TRNS,  KC_TRNS,  KC_TRNS,  RGB_TOG,  KC_TFT_NEXT, KC_NO   },
    { KC_TRNS,     KC_TRNS,  KC_TRNS,  KC_TRNS,  MACRO(0),   MACRO(1),    MACRO(2),  MACRO(3),  MACRO(4), KC_MACRO_SPD_DN, KC_MACRO_SPD_UP, KC_TRNS,  KC_TRNS,  RGB_VAI,  KC_NO,   KC_NO   },
    { KC_TRNS,     KC_WIN_LOCK,KC_TRNS,KC_NO,    KC_NO,      KC_NO,       KC_TFT_OK, KC_NO,     KC_NO,    KC_MAC_TOG,  KC_TRNS,  TG(3),    RGB_RMOD, RGB_VAD,  RGB_MOD, KC_NO   },
  },
  /* Layer 2: macOS ISO Mode (LAlt<->LGUI swap, Mac Fn behavior) */
  {
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO,   KC_TRNS },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO,   KC_TRNS, KC_TRNS },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO   },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO,   KC_NO   },
    { KC_TRNS, KC_LALT, KC_LGUI, KC_NO,   KC_NO,   KC_NO,   KC_TRNS, KC_NO,   KC_NO,   KC_RGUI, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO   },
  },
  /* Layer 3: Custom / Gaming Layer (all TRNS by default so VIA edits layer smoothly) */
  {
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO,   KC_TRNS },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO,   KC_TRNS, KC_TRNS },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO   },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO,   KC_NO   },
    { KC_TRNS, KC_TRNS, KC_TRNS, KC_NO,   KC_NO,   KC_NO,   KC_TRNS, KC_NO,   KC_NO,   KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_TRNS, KC_NO   },
  },
};

static uint16_t const k_default_encoders[TK75_KEYMAP_LAYERS][2] = {
  { KC_VOLD, KC_VOLU }, /* Layer 0 */
  { RGB_VAD, RGB_VAI }, /* Layer 1 (Fn+Knob) */
  { KC_TRNS, KC_TRNS }, /* Layer 2 */
  { KC_TRNS, KC_TRNS }, /* Layer 3 */
};

void keymap_copy_default_layers(uint16_t dst[TK75_KEYMAP_LAYERS][TK75_KEYMAP_ROWS][TK75_KEYMAP_COLS])
{
  memcpy(dst, k_default_layers, sizeof(k_default_layers));
}

void keymap_copy_default_encoders(uint16_t dst[TK75_KEYMAP_LAYERS][2])
{
  memcpy(dst, k_default_encoders, sizeof(k_default_encoders));
}

static uint16_t qmk_to_consumer_usage(uint16_t kc)
{
  switch (kc) {
    case KC_MUTE: return 0x00E2U;
    case KC_VOLU: return 0x00E9U;
    case KC_VOLD: return 0x00EAU;
    case KC_MNXT: return 0x00B5U;
    case KC_MPRV: return 0x00B6U;
    case KC_MSTP: return 0x00B7U;
    case KC_MPLY: return 0x00CDU;
    case KC_BRID: return 0x0070U;
    case KC_BRIU: return 0x006FU;
    default:      return 0U;
  }
}

static uint8_t qmk_to_system_usage(uint16_t kc)
{
  switch (kc) {
    case KC_PWR:  return 0x81U;
    case KC_SLEP: return 0x82U;
    case KC_WAKE: return 0x83U;
    default:      return 0U;
  }
}

static bool is_special_action(uint16_t kc)
{
  return ((kc & 0xFFE0U) == QK_TO_BASE) ||
         ((kc & 0xFFE0U) == QK_MO_BASE) ||
         ((kc & 0xFFE0U) == QK_TG_BASE) ||
         ((kc & 0xFFF0U) == QK_MACRO_BASE) ||
         (kc >= RGB_TOG && kc <= 0x7FFFU);
}

static void append_hid_code(keymap_report_t *rep, uint8_t code)
{
  if (code == 0U || code >= 0x80U) {
    return;
  }
  uint8_t byte_idx = code >> 3U;
  uint8_t bit_mask = (uint8_t)(1U << (code & 7U));
  if ((rep->nkro_bitmap[byte_idx] & bit_mask) == 0U) {
    rep->nkro_bitmap[byte_idx] |= bit_mask;
    if (rep->pressed_count < 6U) {
      rep->boot_keys[rep->pressed_count] = code;
    }
    rep->pressed_count++;
  }
}

static void emit_keycode(keymap_engine_t *km, uint16_t kc, keymap_report_t *rep)
{
  if (kc == KC_NO || kc == KC_TRNS) {
    return;
  }

  /* Block LGUI/RGUI when Win-Lock is enabled */
  if (km->cfg->win_lock != 0U && (kc == KC_LGUI || kc == KC_RGUI)) {
    return;
  }

  if (kc >= 0x00E0U && kc <= 0x00E7U) {
    rep->modifiers |= (uint8_t)(1U << (kc - 0x00E0U));
    return;
  }

  uint16_t cons = qmk_to_consumer_usage(kc);
  if (cons != 0U) {
    rep->consumer_usage = cons;
    return;
  }

  uint8_t sys = qmk_to_system_usage(kc);
  if (sys != 0U) {
    rep->system_usage = sys;
    return;
  }

  /* QMK Modified basic keycodes: 0x0100..0x1FFF */
  if (kc >= 0x0100U && kc <= 0x1FFFU) {
    uint8_t mod_bits = (uint8_t)((kc >> 8U) & 0x0FU);
    bool is_right    = ((kc & 0x1000U) != 0U);
    rep->modifiers  |= is_right ? (uint8_t)(mod_bits << 4U) : mod_bits;
    append_hid_code(rep, (uint8_t)(kc & 0xFFU));
    return;
  }

  if (kc < 0x00E0U) {
    append_hid_code(rep, (uint8_t)kc);
  }
}

uint16_t keymap_lookup(tk75_persist_cfg_t const *cfg, uint32_t active_mask, uint8_t row, uint8_t col)
{
  if (row >= TK75_KEYMAP_ROWS || col >= TK75_KEYMAP_COLS) {
    return KC_NO;
  }

  for (int8_t l = (int8_t)(TK75_KEYMAP_LAYERS - 1U); l >= 0; l--) {
    if ((active_mask & (1UL << (uint8_t)l)) == 0U && l != 0) {
      continue;
    }
    uint16_t kc = cfg->keymap[l][row][col];
    if (kc != KC_TRNS) {
      return kc;
    }
  }
  return KC_NO;
}

static uint8_t highest_layer(uint32_t mask)
{
  for (int8_t l = (int8_t)(TK75_KEYMAP_LAYERS - 1U); l >= 0; l--) {
    if ((mask & (1UL << (uint8_t)l)) != 0U) {
      return (uint8_t)l;
    }
  }
  return 0U;
}

static void handle_layer_key(keymap_engine_t *km, uint16_t kc, bool pressed)
{
  uint8_t target = (uint8_t)(kc & 0x03U);
  if ((kc & 0xFFE0U) == QK_MO_BASE) {
    if (pressed) {
      km->layer_state |= (1UL << target);
    } else {
      km->layer_state &= ~(1UL << target);
    }
  } else if ((kc & 0xFFE0U) == QK_TG_BASE && pressed) {
    km->layer_state ^= (1UL << target);
  } else if ((kc & 0xFFE0U) == QK_TO_BASE && pressed) {
    km->layer_state = (target == 0U) ? 1UL : ((1UL << target) | 1UL);
  }
  if (pressed && (((kc & 0xFFE0U) == QK_TG_BASE && target == 2U) || (kc & 0xFFE0U) == QK_TO_BASE)) {
    km->mac_mode = ((km->layer_state & (1UL << 2U)) != 0U);
    if (km->cfg != NULL) {
      km->cfg->mac_mode = km->mac_mode ? 1U : 0U;
    }
  }
}

void keymap_engine_init(keymap_engine_t *km, tk75_persist_cfg_t *cfg, hall_engine_t *hall)
{
  memset(km, 0, sizeof(*km));
  km->cfg         = cfg;
  km->hall        = hall;
  km->layer_state = 1UL; /* Layer 0 always active base */
}

void keymap_set_action_cb(keymap_engine_t *km, keymap_action_cb_t cb, void *ctx)
{
  km->action_cb  = cb;
  km->action_ctx = ctx;
}

void keymap_set_mac_mode(keymap_engine_t *km, bool is_mac)
{
  km->mac_mode = is_mac;
  if (is_mac) {
    km->layer_state |= (1UL << 2U);
  } else {
    km->layer_state &= ~(1UL << 2U);
  }
}

static uint16_t lookup_encoder_keycode(keymap_engine_t const *km, uint8_t dir_idx)
{
  uint32_t active_mask = km->layer_state | 1UL;
  for (int8_t l = (int8_t)(TK75_KEYMAP_LAYERS - 1U); l >= 0; l--) {
    if ((active_mask & (1UL << (uint8_t)l)) == 0U && l != 0) {
      continue;
    }
    uint16_t kc = km->cfg->encoders[l][dir_idx];
    if (kc != KC_TRNS) {
      return kc;
    }
  }
  return KC_NO;
}

bool keymap_try_encoder_overlay_action(keymap_engine_t *km)
{
  if (km == NULL || km->cfg == NULL) {
    return false;
  }
  for (int8_t l = (int8_t)(TK75_KEYMAP_LAYERS - 1U); l >= 1; l--) {
    if ((km->layer_state & (1UL << (uint8_t)l)) == 0U) {
      continue;
    }
    uint16_t kc = km->cfg->keymap[l][2U][15U];
    if (kc == KC_TRNS) {
      continue;
    }
    if (is_special_action(kc) && km->action_cb != NULL) {
      km->action_cb(kc, true, km->action_ctx);
      return true;
    }
    return false;
  }
  return false;
}

void keymap_reset_encoder_state(keymap_engine_t *km)
{
  if (km == NULL) {
    return;
  }
  km->enc_prev_btn    = false;
  km->enc_pulse_ticks = 0U;
  km->enc_pulse_dir   = 0;
  km->pulse_consumer  = 0U;
}

void keymap_push_encoder(keymap_engine_t *km, int8_t delta, bool btn_pressed)
{
  if (delta != 0) {
    uint16_t kc = lookup_encoder_keycode(km, (delta > 0) ? 1U : 0U);
    if (is_special_action(kc)) {
      if (km->action_cb != NULL) {
        int8_t steps = (delta > 0) ? delta : -delta;
        for (int8_t s = 0; s < steps; s++) {
          km->action_cb(kc, true, km->action_ctx);
        }
      }
    } else {
      uint16_t cons = qmk_to_consumer_usage(kc);
      if (cons != 0U) {
        km->pulse_consumer = cons;
      } else {
        km->enc_pulse_dir   = delta;
        km->enc_pulse_ticks = 8U;
      }
    }
  }

  /* Virtual encoder switch sits at (row=2, col=15) matching VIA layout */
  if (btn_pressed != km->enc_prev_btn) {
    km->enc_prev_btn = btn_pressed;
    uint16_t kc = keymap_lookup(km->cfg, km->layer_state | 1UL, 2U, 15U);
    if (btn_pressed && is_special_action(kc) && km->action_cb != NULL) {
      km->action_cb(kc, true, km->action_ctx);
    }
  }
}

void keymap_build_report(keymap_engine_t *km, keymap_report_t *out)
{
  memset(out, 0, sizeof(*out));
  uint32_t layer_mask = km->layer_state | 1UL;

  /* 1. Process edge transitions first so MO/TG layers apply immediately on this scan */
  for (uint8_t r = 0U; r < TK75_KEYMAP_ROWS; r++) {
    for (uint8_t c = 0U; c < HALL_COLS; c++) {
      uint32_t slot = ((uint32_t)r * HALL_COLS) + c;
      bool now  = hall_engine_is_pressed(km->hall, r, c);
      bool prev = (km->prev_pressed[slot] != 0U);

      if (now && !prev) {
        uint16_t kc = keymap_lookup(km->cfg, layer_mask, r, c);
        km->active_codes[slot] = kc;
        if (is_special_action(kc)) {
          handle_layer_key(km, kc, true);
          if (km->action_cb != NULL) {
            km->action_cb(kc, true, km->action_ctx);
          }
        }
      } else if (!now && prev) {
        uint16_t kc = km->active_codes[slot];
        if (is_special_action(kc)) {
          handle_layer_key(km, kc, false);
          if (km->action_cb != NULL) {
            km->action_cb(kc, false, km->action_ctx);
          }
        }
        km->active_codes[slot] = KC_NO;
      }
      km->prev_pressed[slot] = now ? 1U : 0U;
    }
  }

  layer_mask = km->layer_state | 1UL;
  out->active_layer = highest_layer(layer_mask);

  /* 2. Emit held basic/modifier/consumer keycodes using press-latched keycodes (prevents stuck keys on Fn release) */
  for (uint32_t slot = 0U; slot < HALL_SLOT_COUNT; slot++) {
    if (km->prev_pressed[slot] != 0U) {
      uint16_t kc = km->active_codes[slot];
      if (!is_special_action(kc)) {
        emit_keycode(km, kc, out);
      }
    }
  }

  /* 3. Include encoder push or temporary consumer pulse */
  if (km->enc_prev_btn) {
    uint16_t kc = keymap_lookup(km->cfg, layer_mask, 2U, 15U);
    if (!is_special_action(kc)) {
      emit_keycode(km, kc, out);
    }
  }
  if (km->pulse_consumer != 0U) {
    out->consumer_usage = km->pulse_consumer;
    km->pulse_consumer  = 0U;
  } else if (km->enc_pulse_ticks > 0U) {
    uint16_t kc = keymap_lookup(km->cfg, layer_mask, (km->enc_pulse_dir > 0) ? 1U : 0U, 15U);
    emit_keycode(km, kc, out);
    km->enc_pulse_ticks--;
  }

  if (km->cfg->win_lock != 0U) {
    out->modifiers &= (uint8_t)~((1U << 3U) | (1U << 7U));
  }
}
