#ifndef MACRO_PLAYER_H
#define MACRO_PLAYER_H

#include "flash_eeprom.h"
#include "keymap.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MACRO_MAX_CONCURRENT_KEYS 6U
#define MACRO_TYPED_BITMAP_BYTES  32U /* 256 HID keycodes (0x00..0xFF) */

typedef enum {
  MACRO_SPEED_FAST = 1, /* 2ms tap / 2ms gap / 0.25x delay */
  MACRO_SPEED_NORM = 2, /* 10ms tap / 8ms gap / 1x delay (default) */
  MACRO_SPEED_MED  = 3, /* 25ms tap / 15ms gap / 2x delay */
  MACRO_SPEED_SLOW = 4, /* 50ms tap / 25ms gap / 4x delay */
  MACRO_SPEED_SAFE = 5, /* 100ms tap / 40ms gap / 8x delay */
} macro_speed_preset_t;

typedef struct {
  bool     active;
  uint8_t  speed_idx; /* macro_speed_preset_t (0 normalizes to MACRO_SPEED_NORM) */
  uint16_t cursor;
  uint32_t delay_until_ms;
  uint8_t  held_mods;
  uint8_t  held_keys[MACRO_MAX_CONCURRENT_KEYS];
  uint8_t  pending_tap_key;
  bool     tap_auto_shift;
  uint8_t  typed_pulse_bitmap[MACRO_TYPED_BITMAP_BYTES];
} macro_player_t;

/** Initialize macro player state. */
void macro_player_init(macro_player_t *mp);

/** Normalize stored speed index (0 or out-of-range -> MACRO_SPEED_NORM). */
uint8_t macro_player_normalize_speed(uint8_t speed_idx);

/** Set macro playback speed preset (MACRO_SPEED_FAST..MACRO_SPEED_SAFE). */
void macro_player_set_speed(macro_player_t *mp, uint8_t speed_idx);

/** Short 6-char HUD label for speed preset (e.g. "M:FAST", "M:NORM"). */
char const *macro_player_speed_label(uint8_t speed_idx);

/** Trigger macro slot 0..15 from null-delimited VIA macro buffer. */
void macro_player_trigger(macro_player_t *mp, uint8_t const *macro_buf, uint8_t macro_id, uint32_t now_ms);

/** Advance active macro script and merge any virtual key presses into HID report. */
void macro_player_step(macro_player_t *mp, uint8_t const *macro_buf, uint32_t now_ms, keymap_report_t *rep);

/** Copy currently held + newly tapped macro HID codes into out_bitmap and clear tap latch. */
void macro_player_consume_typed_bitmap(macro_player_t *mp, uint8_t out_bitmap[MACRO_TYPED_BITMAP_BYTES]);

#ifdef __cplusplus
}
#endif

#endif /* MACRO_PLAYER_H */
