#include "macro_player.h"
#include <string.h>

/* QMK VIA v3 macro bytecode escapes */
#define SS_QMK_PREFIX 0x01U
#define SS_TAP_CODE   0x01U
#define SS_DOWN_CODE  0x02U
#define SS_UP_CODE    0x03U
#define SS_DELAY_CODE 0x04U

static uint8_t ascii_to_hid(uint8_t ch, bool *out_shift)
{
  *out_shift = false;
  if (ch >= 'a' && ch <= 'z') {
    return (uint8_t)(KC_A + (ch - 'a'));
  }
  if (ch >= 'A' && ch <= 'Z') {
    *out_shift = true;
    return (uint8_t)(KC_A + (ch - 'A'));
  }
  if (ch >= '1' && ch <= '9') {
    return (uint8_t)(KC_1 + (ch - '1'));
  }
  if (ch == '0') {
    return (uint8_t)KC_0;
  }
  if (ch == ' ') {
    return (uint8_t)KC_SPC;
  }
  if (ch == '\n' || ch == '\r') {
    return (uint8_t)KC_ENT;
  }
  if (ch == '\t') {
    return (uint8_t)KC_TAB;
  }
  return 0U;
}

typedef struct {
  uint8_t tap_ms;
  uint8_t gap_ms;
  uint8_t chord_ms;
  uint8_t delay_num; /* Multiplier = delay_num / 4 (1=0.25x, 4=1x, 8=2x, 16=4x, 32=8x) */
} macro_speed_profile_t;

static macro_speed_profile_t const k_speed_profiles[5] = {
  {   2U,  2U,  2U,  1U }, /* 1: FAST (2ms / 0.25x) */
  {  10U,  8U,  4U,  4U }, /* 2: NORM (10ms / 1x) */
  {  25U, 15U,  8U,  8U }, /* 3: MED  (25ms / 2x) */
  {  50U, 25U, 15U, 16U }, /* 4: SLOW (50ms / 4x) */
  { 100U, 40U, 25U, 32U }, /* 5: SAFE (100ms / 8x) */
};

uint8_t macro_player_normalize_speed(uint8_t speed_idx)
{
  return (speed_idx >= (uint8_t)MACRO_SPEED_FAST && speed_idx <= (uint8_t)MACRO_SPEED_SAFE)
           ? speed_idx
           : (uint8_t)MACRO_SPEED_NORM;
}

static macro_speed_profile_t const *active_profile(macro_player_t const *mp)
{
  uint8_t idx = macro_player_normalize_speed(mp->speed_idx);
  return &k_speed_profiles[idx - 1U];
}

void macro_player_set_speed(macro_player_t *mp, uint8_t speed_idx)
{
  if (mp != NULL) {
    mp->speed_idx = macro_player_normalize_speed(speed_idx);
  }
}

char const *macro_player_speed_label(uint8_t speed_idx)
{
  static char const *const k_labels[5] = {
    "M:FAST", "M:NORM", "M:MED ", "M:SLOW", "M:SAFE",
  };
  uint8_t idx = macro_player_normalize_speed(speed_idx);
  return k_labels[idx - 1U];
}

static void pulse_typed_code(macro_player_t *mp, uint8_t code)
{
  if (code > 0U) {
    mp->typed_pulse_bitmap[code >> 3U] |= (uint8_t)(1U << (code & 7U));
  }
}

static void add_held_key(macro_player_t *mp, uint8_t code)
{
  pulse_typed_code(mp, code);
  if (code >= 0xE0U && code <= 0xE7U) {
    mp->held_mods |= (uint8_t)(1U << (code - 0xE0U));
    return;
  }
  for (uint8_t i = 0U; i < MACRO_MAX_CONCURRENT_KEYS; i++) {
    if (mp->held_keys[i] == code) {
      return;
    }
    if (mp->held_keys[i] == 0U) {
      mp->held_keys[i] = code;
      return;
    }
  }
}

static void remove_held_key(macro_player_t *mp, uint8_t code)
{
  if (code >= 0xE0U && code <= 0xE7U) {
    mp->held_mods &= (uint8_t)(~(1U << (code - 0xE0U)));
    return;
  }
  for (uint8_t i = 0U; i < MACRO_MAX_CONCURRENT_KEYS; i++) {
    if (mp->held_keys[i] == code) {
      mp->held_keys[i] = 0U;
    }
  }
}

static uint16_t find_macro_offset(uint8_t const *buf, uint8_t macro_id)
{
  uint16_t off = 0U;
  uint8_t cur_id = 0U;

  while (cur_id < macro_id && off < TK75_MACRO_BUF_BYTES) {
    if (buf[off] == 0U) {
      cur_id++;
    }
    off++;
  }
  return (cur_id == macro_id && off < TK75_MACRO_BUF_BYTES) ? off : TK75_MACRO_BUF_BYTES;
}

void macro_player_init(macro_player_t *mp)
{
  memset(mp, 0, sizeof(*mp));
  mp->speed_idx = (uint8_t)MACRO_SPEED_NORM;
}

void macro_player_trigger(macro_player_t *mp, uint8_t const *macro_buf, uint8_t macro_id, uint32_t now_ms)
{
  if (macro_buf == NULL || macro_id >= TK75_MACRO_COUNT) {
    return;
  }
  uint16_t off = find_macro_offset(macro_buf, macro_id);
  if (off >= TK75_MACRO_BUF_BYTES || macro_buf[off] == 0U) {
    return;
  }
  uint8_t saved_speed = macro_player_normalize_speed(mp->speed_idx);
  memset(mp, 0, sizeof(*mp));
  mp->speed_idx      = saved_speed;
  mp->active         = true;
  mp->cursor         = off;
  mp->delay_until_ms = now_ms;
}

void macro_player_consume_typed_bitmap(macro_player_t *mp, uint8_t out_bitmap[MACRO_TYPED_BITMAP_BYTES])
{
  memset(out_bitmap, 0, MACRO_TYPED_BITMAP_BYTES);
  if (mp == NULL) {
    return;
  }
  memcpy(out_bitmap, mp->typed_pulse_bitmap, MACRO_TYPED_BITMAP_BYTES);
  memset(mp->typed_pulse_bitmap, 0, MACRO_TYPED_BITMAP_BYTES);

  if (!mp->active) {
    return;
  }
  for (uint8_t bit = 0U; bit < 8U; bit++) {
    if ((mp->held_mods & (uint8_t)(1U << bit)) != 0U) {
      uint8_t code = (uint8_t)(0xE0U + bit);
      out_bitmap[code >> 3U] |= (uint8_t)(1U << (code & 7U));
    }
  }
  for (uint8_t i = 0U; i < MACRO_MAX_CONCURRENT_KEYS; i++) {
    uint8_t code = mp->held_keys[i];
    if (code > 0U) {
      out_bitmap[code >> 3U] |= (uint8_t)(1U << (code & 7U));
    }
  }
}

static void merge_into_report(macro_player_t const *mp, keymap_report_t *rep)
{
  rep->modifiers |= mp->held_mods;
  for (uint8_t i = 0U; i < MACRO_MAX_CONCURRENT_KEYS; i++) {
    uint8_t k = mp->held_keys[i];
    if (k > 0U && k < 0x80U) {
      rep->nkro_bitmap[k >> 3U] |= (uint8_t)(1U << (k & 7U));
      if (rep->pressed_count < 6U) {
        rep->boot_keys[rep->pressed_count] = k;
      }
      rep->pressed_count++;
    }
  }
}

void macro_player_step(macro_player_t *mp, uint8_t const *macro_buf, uint32_t now_ms, keymap_report_t *rep)
{
  if (!mp->active || macro_buf == NULL) {
    return;
  }

  if ((int32_t)(now_ms - mp->delay_until_ms) < 0) {
    merge_into_report(mp, rep);
    return;
  }

  macro_speed_profile_t const *prof = active_profile(mp);

  /* Release synthetic key from previous tap tick before reading next token */
  if (mp->pending_tap_key != 0U) {
    remove_held_key(mp, mp->pending_tap_key);
    if (mp->tap_auto_shift) {
      mp->held_mods     &= (uint8_t)(~(1U << 1U));
      mp->tap_auto_shift = false;
    }
    mp->pending_tap_key = 0U;
    mp->delay_until_ms  = now_ms + prof->gap_ms;
    merge_into_report(mp, rep);
    return;
  }

  if (mp->cursor >= TK75_MACRO_BUF_BYTES || macro_buf[mp->cursor] == 0U) {
    uint8_t saved_speed = macro_player_normalize_speed(mp->speed_idx);
    uint8_t saved_pulse[MACRO_TYPED_BITMAP_BYTES];
    memcpy(saved_pulse, mp->typed_pulse_bitmap, sizeof(saved_pulse));
    memset(mp, 0, sizeof(*mp));
    mp->speed_idx = saved_speed;
    memcpy(mp->typed_pulse_bitmap, saved_pulse, sizeof(saved_pulse));
    return;
  }

  uint8_t token = macro_buf[mp->cursor++];
  if (token == SS_QMK_PREFIX && mp->cursor < TK75_MACRO_BUF_BYTES) {
    uint8_t action = macro_buf[mp->cursor++];
    uint8_t arg    = (mp->cursor < TK75_MACRO_BUF_BYTES) ? macro_buf[mp->cursor++] : 0U;
    if (action == SS_TAP_CODE) {
      add_held_key(mp, arg);
      mp->pending_tap_key = arg;
      mp->delay_until_ms  = now_ms + prof->tap_ms;
    } else if (action == SS_DOWN_CODE) {
      add_held_key(mp, arg);
      mp->delay_until_ms = now_ms + prof->chord_ms;
    } else if (action == SS_UP_CODE) {
      remove_held_key(mp, arg);
      mp->delay_until_ms = now_ms + prof->chord_ms;
    } else if (action == SS_DELAY_CODE) {
      uint8_t  d2     = (mp->cursor < TK75_MACRO_BUF_BYTES) ? macro_buf[mp->cursor++] : 0U;
      uint16_t raw_ms = (uint16_t)((uint16_t)arg | ((uint16_t)d2 << 8U));
      uint32_t scaled = ((uint32_t)raw_ms * prof->delay_num) >> 2U;
      if (raw_ms > 0U && scaled == 0U) {
        scaled = 1U;
      }
      mp->delay_until_ms = now_ms + scaled;
    }
  } else if (token != SS_QMK_PREFIX) {
    bool shift = false;
    uint8_t hid = ascii_to_hid(token, &shift);
    if (hid != 0U) {
      if (shift && (mp->held_mods & (1U << 1U)) == 0U) {
        mp->held_mods     |= (1U << 1U);
        mp->tap_auto_shift = true;
        pulse_typed_code(mp, (uint8_t)KC_LSFT);
      }
      add_held_key(mp, hid);
      mp->pending_tap_key = hid;
      mp->delay_until_ms  = now_ms + prof->tap_ms;
    }
  }
  merge_into_report(mp, rep);
}
