#ifndef MACRO_RECORDER_H
#define MACRO_RECORDER_H

#include "default_keymap_iso.h"
#include "flash_eeprom.h"
#include "hall_engine.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MACRO_REC_HOLD_MS        1800U
#define MACRO_REC_SAVED_FLASH_MS 600U
#define MACRO_REC_MAX_BYTES      1024U /* Up to 341 recorded key events per single K5 macro */
#define MACRO_EDITOR_MAX_TOKS    256U  /* Editable atomic key/combo tokens in TFT review editor */
#define MACRO_EDITOR_COLS        19U   /* Visible columns per line in 128x128 TFT editor */

typedef struct {
  uint8_t hid;  /* HID keycode (0x04..0x7F or 0xE0..0xE7) */
  uint8_t mods; /* Held modifier bitmask (bit0..3=LCtl..LGui, bit4..7=RCtl..RGui) */
} macro_edit_tok_t;

typedef enum {
  MACRO_REC_IDLE = 0,
  MACRO_REC_WAIT_START_RELEASE = 1,
  MACRO_REC_RECORDING = 2,
  MACRO_REC_WAIT_REVIEW_RELEASE = 3,
  MACRO_REC_REVIEW = 4,
  MACRO_REC_WAIT_STOP_RELEASE = 5,
} macro_rec_state_t;

typedef enum {
  MACRO_REC_EVT_NONE = 0,
  MACRO_REC_EVT_STARTED = 1,
  MACRO_REC_EVT_REVIEW = 2,
  MACRO_REC_EVT_SAVED = 3,
  MACRO_REC_EVT_CANCELLED = 4,
  MACRO_REC_EVT_PLAY_SLOT = 5,
} macro_rec_evt_t;

typedef struct {
  uint8_t  macro_id;
  bool     pressed;
  bool     fn_active;
  uint32_t now_ms;
} macro_key_event_t;

typedef struct {
  hall_engine_t const *hall;
  tk75_persist_cfg_t  *cfg;
  uint32_t             layer_state;
  uint32_t             now_ms;
  bool                 tft_review_enabled; /* True when Pixel HUD TFT is on -> review/edit before save */
} macro_rec_tick_ctx_t;

typedef struct {
  macro_rec_state_t state;
  bool              hold_armed;
  uint8_t           hold_slot;
  uint32_t          hold_start_ms;
  uint8_t           rec_slot;
  uint8_t           buf[MACRO_REC_MAX_BYTES];
  uint16_t          len;
  uint8_t           rec_held_mods;
  uint8_t           mod_used_mask;
  uint8_t           prev_pressed[HALL_SLOT_COUNT];
  uint8_t           saved_slot;
  uint32_t          saved_until_ms;
  uint8_t           play_slot;

  /* TFT text-editor review state (active when state == MACRO_REC_REVIEW) */
  macro_edit_tok_t  tokens[MACRO_EDITOR_MAX_TOKS];
  uint16_t          tok_count;
  uint16_t          cursor;
  uint16_t          edit_rev;
  bool              edit_dirty;
  uint8_t           repeat_hid;
  uint32_t          repeat_next_ms;
} macro_recorder_t;

/** Initialize Xtrfy K5-style on-the-fly macro recorder state. */
void macro_recorder_init(macro_recorder_t *rec);

/** Handle press/release of a QK_MACRO_n keycode; returns true if macro should play immediately. */
bool macro_recorder_on_macro_key(macro_recorder_t *rec, macro_key_event_t const *ev);

/** True while recorder or TFT review editor owns keyboard input (suppresses host HID). */
bool macro_recorder_is_capturing(macro_recorder_t const *rec);

/** True while fullscreen TFT macro text editor is active (review/edit before save). */
bool macro_recorder_is_reviewing(macro_recorder_t const *rec);

/** Move TFT editor cursor by signed delta (e.g. rotary knob scrub). */
void macro_recorder_editor_move(macro_recorder_t *rec, int16_t delta);

/** Discard TFT editor changes without overwriting saved EEPROM slot. */
macro_rec_evt_t macro_recorder_editor_cancel(macro_recorder_t *rec, hall_engine_t const *hall);

/** Parse VIA v3 macro bytecode into editable key/combo tokens. */
uint16_t macro_recorder_parse_via_tokens(uint8_t const *buf, uint16_t len,
                                         macro_edit_tok_t *out_toks, uint16_t max_toks);

/** Serialize editable tokens back into compact native VIA v3 bytecode. */
uint16_t macro_recorder_serialize_tokens(macro_edit_tok_t const *toks, uint16_t count,
                                         uint8_t *out_buf, uint16_t max_bytes);

/** True if macro slot 0..15 contains at least 1 byte of VIA bytecode. */
bool macro_recorder_slot_has_data(uint8_t const *macro_buf, uint8_t slot);

/** Splice bytecode into null-delimited VIA macro slot 0..15, preserving all other slots. */
bool macro_recorder_save_slot(uint8_t macro_buf[TK75_MACRO_BUF_BYTES],
                              uint8_t slot,
                              uint8_t const *data,
                              uint16_t len);

/** Advance hold timer and capture keystrokes while recording. */
macro_rec_evt_t macro_recorder_tick(macro_recorder_t *rec, macro_rec_tick_ctx_t const *ctx);

#ifdef __cplusplus
}
#endif

#endif /* MACRO_RECORDER_H */
