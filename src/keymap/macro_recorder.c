#include "macro_recorder.h"
#include <string.h>

#define SS_QMK_PREFIX         0x01U
#define SS_TAP_CODE           0x01U
#define SS_DOWN_CODE          0x02U
#define SS_UP_CODE            0x03U
#define SS_DELAY_CODE         0x04U
#define EDITOR_REPEAT_INIT_MS 360U
#define EDITOR_REPEAT_STEP_MS 65U

static bool is_mod_hid(uint8_t code)
{
  return (code >= 0xE0U && code <= 0xE7U);
}

static bool is_recordable_hid(uint16_t kc)
{
  return (kc >= 0x0004U && kc <= 0x007FU) || (kc >= 0x00E0U && kc <= 0x00E7U);
}

static bool is_fn_stop_key(tk75_persist_cfg_t const *cfg, uint8_t row, uint8_t col)
{
  return cfg->keymap[0][row][col] == MO(1);
}

static uint16_t lookup_record_kc(tk75_persist_cfg_t const *cfg, uint8_t row, uint8_t col)
{
  if (cfg->mac_mode != 0U) {
    uint16_t mac_kc = cfg->keymap[2][row][col];
    if (mac_kc != KC_TRNS) {
      return mac_kc;
    }
  }
  return cfg->keymap[0][row][col];
}

static void snapshot_pressed(macro_recorder_t *rec, hall_engine_t const *hall)
{
  for (uint8_t r = 0U; r < TK75_KEYMAP_ROWS; r++) {
    for (uint8_t c = 0U; c < HALL_COLS; c++) {
      uint32_t slot = ((uint32_t)r * HALL_COLS) + c;
      rec->prev_pressed[slot] = hall_engine_is_pressed(hall, r, c) ? 1U : 0U;
    }
  }
}

static bool any_key_pressed(hall_engine_t const *hall)
{
  for (uint8_t r = 0U; r < TK75_KEYMAP_ROWS; r++) {
    for (uint8_t c = 0U; c < HALL_COLS; c++) {
      if (hall_engine_is_pressed(hall, r, c)) {
        return true;
      }
    }
  }
  return false;
}

static void append_event(macro_recorder_t *rec, uint8_t op, uint8_t hid_code)
{
  if ((uint16_t)(rec->len + 3U) > MACRO_REC_MAX_BYTES) {
    return;
  }
  rec->buf[rec->len++] = SS_QMK_PREFIX;
  rec->buf[rec->len++] = op;
  rec->buf[rec->len++] = hid_code;
}

static void release_remaining_mods(macro_recorder_t *rec)
{
  for (uint8_t bit = 0U; bit < 8U; bit++) {
    if ((rec->rec_held_mods & (uint8_t)(1U << bit)) != 0U) {
      append_event(rec, SS_UP_CODE, (uint8_t)(0xE0U + bit));
      rec->rec_held_mods &= (uint8_t)(~(1U << bit));
    }
  }
}

static uint16_t clamp_safe_bytecode_len(uint8_t const *data, uint16_t len, uint16_t max_len)
{
  if (len <= max_len) {
    return len;
  }
  uint16_t safe = 0U;
  while (safe < len) {
    uint16_t step = 1U;
    if (data[safe] == SS_QMK_PREFIX && (uint16_t)(safe + 2U) < len) {
      step = (data[safe + 1U] == 0x04U) ? 4U : 3U;
    }
    if ((uint16_t)(safe + step) > max_len) {
      break;
    }
    safe = (uint16_t)(safe + step);
  }
  return safe;
}

void macro_recorder_init(macro_recorder_t *rec)
{
  if (rec != NULL) {
    memset(rec, 0, sizeof(*rec));
  }
}

bool macro_recorder_is_capturing(macro_recorder_t const *rec)
{
  return (rec != NULL) && (rec->state != MACRO_REC_IDLE);
}

bool macro_recorder_is_reviewing(macro_recorder_t const *rec)
{
  return (rec != NULL) &&
         (rec->state == MACRO_REC_REVIEW || rec->state == MACRO_REC_WAIT_REVIEW_RELEASE);
}

void macro_recorder_editor_move(macro_recorder_t *rec, int16_t delta)
{
  if (rec == NULL || rec->state != MACRO_REC_REVIEW || delta == 0) {
    return;
  }
  int32_t next = (int32_t)rec->cursor + (int32_t)delta;
  if (next < 0) {
    next = 0;
  } else if (next > (int32_t)rec->tok_count) {
    next = (int32_t)rec->tok_count;
  }
  if ((uint16_t)next != rec->cursor) {
    rec->cursor = (uint16_t)next;
    rec->edit_rev++;
  }
}

macro_rec_evt_t macro_recorder_editor_cancel(macro_recorder_t *rec, hall_engine_t const *hall)
{
  if (rec == NULL || !macro_recorder_is_reviewing(rec)) {
    return MACRO_REC_EVT_NONE;
  }
  rec->repeat_hid    = 0U;
  rec->rec_held_mods = 0U;
  rec->state         = MACRO_REC_WAIT_STOP_RELEASE;
  if (hall != NULL) {
    snapshot_pressed(rec, hall);
  }
  return MACRO_REC_EVT_CANCELLED;
}

uint16_t macro_recorder_parse_via_tokens(uint8_t const *buf, uint16_t len,
                                         macro_edit_tok_t *out_toks, uint16_t max_toks)
{
  if (buf == NULL || out_toks == NULL || max_toks == 0U) {
    return 0U;
  }
  uint16_t out = 0U;
  uint16_t i   = 0U;
  uint8_t  mods = 0U;
  uint8_t  mod_used = 0U;

  while (i < len && buf[i] != 0U) {
    if (buf[i] == SS_QMK_PREFIX && (uint16_t)(i + 2U) < len) {
      uint8_t op   = buf[i + 1U];
      uint8_t code = buf[i + 2U];
      if (op == SS_DELAY_CODE) {
        i = (uint16_t)(i + (((uint16_t)(i + 3U) < len) ? 4U : 3U));
        continue;
      }
      i = (uint16_t)(i + 3U);
      if (op == SS_DOWN_CODE) {
        if (is_mod_hid(code)) {
          uint8_t bit = (uint8_t)(1U << (code - 0xE0U));
          mods     |= bit;
          mod_used &= (uint8_t)(~bit);
        } else if (out < max_toks) {
          out_toks[out].hid  = code;
          out_toks[out].mods = mods;
          out++;
          mod_used |= mods;
        }
      } else if (op == SS_UP_CODE) {
        if (is_mod_hid(code)) {
          uint8_t bit = (uint8_t)(1U << (code - 0xE0U));
          mods &= (uint8_t)(~bit);
          if ((mod_used & bit) == 0U && out < max_toks) {
            out_toks[out].hid  = code;
            out_toks[out].mods = mods;
            out++;
          }
          mod_used &= (uint8_t)(~bit);
        }
      } else if (op == SS_TAP_CODE) {
        if (out < max_toks) {
          out_toks[out].hid  = code;
          out_toks[out].mods = is_mod_hid(code) ? 0U : mods;
          out++;
          if (!is_mod_hid(code)) {
            mod_used |= mods;
          }
        }
      }
    } else {
      i++;
    }
  }
  return out;
}

static void emit_via3(uint8_t *out_buf, uint16_t *io_len, uint16_t max_bytes, uint8_t op, uint8_t hid)
{
  if ((uint16_t)(*io_len + 3U) > max_bytes) {
    return;
  }
  out_buf[(*io_len)++] = SS_QMK_PREFIX;
  out_buf[(*io_len)++] = op;
  out_buf[(*io_len)++] = hid;
}

uint16_t macro_recorder_serialize_tokens(macro_edit_tok_t const *toks, uint16_t count,
                                         uint8_t *out_buf, uint16_t max_bytes)
{
  if (toks == NULL || out_buf == NULL || max_bytes < 3U) {
    return 0U;
  }
  uint16_t out         = 0U;
  uint8_t  active_mods = 0U;

  for (uint16_t i = 0U; i < count; i++) {
    uint8_t target_mods = is_mod_hid(toks[i].hid) ? 0U : toks[i].mods;
    /* Release modifiers no longer needed */
    uint8_t to_up = (uint8_t)(active_mods & (uint8_t)(~target_mods));
    for (uint8_t b = 0U; b < 8U; b++) {
      if ((to_up & (uint8_t)(1U << b)) != 0U) {
        emit_via3(out_buf, &out, max_bytes, SS_UP_CODE, (uint8_t)(0xE0U + b));
        active_mods &= (uint8_t)(~(1U << b));
      }
    }
    /* Reserve room for closing all target_mods + current tap so bytecode never ends unbalanced */
    uint8_t to_dn = (uint8_t)(target_mods & (uint8_t)(~active_mods));
    uint16_t need = 3U; /* TAP */
    for (uint8_t b = 0U; b < 8U; b++) {
      if ((to_dn & (uint8_t)(1U << b)) != 0U) need = (uint16_t)(need + 3U);
      if ((target_mods & (uint8_t)(1U << b)) != 0U) need = (uint16_t)(need + 3U);
    }
    if ((uint16_t)(out + need) > max_bytes) {
      break;
    }
    for (uint8_t b = 0U; b < 8U; b++) {
      if ((to_dn & (uint8_t)(1U << b)) != 0U) {
        emit_via3(out_buf, &out, max_bytes, SS_DOWN_CODE, (uint8_t)(0xE0U + b));
        active_mods |= (uint8_t)(1U << b);
      }
    }
    emit_via3(out_buf, &out, max_bytes, SS_TAP_CODE, toks[i].hid);
  }

  for (uint8_t b = 0U; b < 8U; b++) {
    if ((active_mods & (uint8_t)(1U << b)) != 0U) {
      emit_via3(out_buf, &out, max_bytes, SS_UP_CODE, (uint8_t)(0xE0U + b));
    }
  }
  return out;
}

bool macro_recorder_slot_has_data(uint8_t const *macro_buf, uint8_t slot)
{
  if (macro_buf == NULL || slot >= TK75_MACRO_COUNT) {
    return false;
  }
  uint16_t off = 0U;
  uint8_t  cur = 0U;
  while (cur < slot && off < TK75_MACRO_BUF_BYTES) {
    if (macro_buf[off] == 0U) {
      cur++;
    }
    off++;
  }
  return (cur == slot && off < TK75_MACRO_BUF_BYTES && macro_buf[off] != 0U);
}

bool macro_recorder_save_slot(uint8_t macro_buf[TK75_MACRO_BUF_BYTES],
                              uint8_t slot,
                              uint8_t const *data,
                              uint16_t len)
{
  if (macro_buf == NULL || slot >= TK75_MACRO_COUNT) {
    return false;
  }

  /* Locate boundaries of all 16 null-delimited VIA macro slots */
  uint16_t starts[TK75_MACRO_COUNT];
  uint16_t lens[TK75_MACRO_COUNT];
  uint16_t pos = 0U;
  for (uint8_t i = 0U; i < TK75_MACRO_COUNT; i++) {
    starts[i] = pos;
    uint16_t n = 0U;
    while (pos < TK75_MACRO_BUF_BYTES && macro_buf[pos] != 0U) {
      pos++;
      n++;
    }
    lens[i] = n;
    if (pos < TK75_MACRO_BUF_BYTES) {
      pos++; /* skip null terminator */
    }
  }

  /* Calculate bytes consumed by all OTHER slots + 16 null terminators */
  uint16_t other_bytes = TK75_MACRO_COUNT;
  for (uint8_t i = 0U; i < TK75_MACRO_COUNT; i++) {
    if (i != slot) {
      other_bytes = (uint16_t)(other_bytes + lens[i]);
    }
  }
  if (other_bytes >= TK75_MACRO_BUF_BYTES) {
    return false;
  }

  uint16_t avail = (uint16_t)(TK75_MACRO_BUF_BYTES - other_bytes);
  uint16_t new_len = (data != NULL) ? clamp_safe_bytecode_len(data, len, avail) : 0U;

  /* Static scratch avoids blowing the 2KB Cortex-M4 stack when splicing 2KB macro pools */
  static uint8_t tmp[TK75_MACRO_BUF_BYTES];
  memset(tmp, 0, sizeof(tmp));
  uint16_t dst = 0U;

  for (uint8_t i = 0U; i < TK75_MACRO_COUNT; i++) {
    if (i == slot) {
      if (new_len > 0U && (uint16_t)(dst + new_len) < TK75_MACRO_BUF_BYTES) {
        memcpy(&tmp[dst], data, new_len);
        dst = (uint16_t)(dst + new_len);
      }
    } else if (lens[i] > 0U && (uint16_t)(dst + lens[i]) < TK75_MACRO_BUF_BYTES) {
      memcpy(&tmp[dst], &macro_buf[starts[i]], lens[i]);
      dst = (uint16_t)(dst + lens[i]);
    }
    if (dst < TK75_MACRO_BUF_BYTES) {
      tmp[dst++] = 0U;
    }
  }

  memcpy(macro_buf, tmp, TK75_MACRO_BUF_BYTES);
  return true;
}

bool macro_recorder_on_macro_key(macro_recorder_t *rec, macro_key_event_t const *ev)
{
  if (rec == NULL || ev == NULL || ev->macro_id >= TK75_MACRO_COUNT) {
    return false;
  }
  if (rec->state != MACRO_REC_IDLE) {
    return false;
  }

  if (ev->pressed) {
    if (ev->fn_active) {
      rec->hold_armed    = true;
      rec->hold_slot     = ev->macro_id;
      rec->hold_start_ms = ev->now_ms;
      return false; /* Wait for release (<1.8s = play) or 1.8s hold (= record) */
    }
    return true; /* Direct macro key outside Fn plays immediately */
  }

  /* Release before 1.8s hold threshold -> play macro cleanly */
  if (rec->hold_armed && rec->hold_slot == ev->macro_id) {
    rec->hold_armed = false;
    return true;
  }
  return false;
}

static macro_rec_evt_t commit_and_exit(macro_recorder_t *rec, macro_rec_tick_ctx_t const *ctx)
{
  (void)macro_recorder_save_slot(ctx->cfg->macros, rec->rec_slot, rec->buf, rec->len);
  rec->saved_slot     = rec->rec_slot;
  rec->saved_until_ms = ctx->now_ms + MACRO_REC_SAVED_FLASH_MS;
  rec->repeat_hid     = 0U;
  rec->rec_held_mods  = 0U;
  rec->state          = MACRO_REC_WAIT_STOP_RELEASE;
  snapshot_pressed(rec, ctx->hall);
  return MACRO_REC_EVT_SAVED;
}

static macro_rec_evt_t finish_recording(macro_recorder_t *rec, macro_rec_tick_ctx_t const *ctx)
{
  release_remaining_mods(rec);
  if (!ctx->tft_review_enabled) {
    return commit_and_exit(rec, ctx);
  }
  rec->tok_count      = macro_recorder_parse_via_tokens(rec->buf, rec->len, rec->tokens, MACRO_EDITOR_MAX_TOKS);
  rec->cursor         = rec->tok_count;
  rec->edit_dirty     = false;
  rec->edit_rev++;
  rec->rec_held_mods  = 0U;
  rec->mod_used_mask  = 0U;
  rec->repeat_hid     = 0U;
  rec->state          = MACRO_REC_WAIT_REVIEW_RELEASE;
  snapshot_pressed(rec, ctx->hall);
  return MACRO_REC_EVT_REVIEW;
}

static bool editor_insert_token(macro_recorder_t *rec, uint8_t hid, uint8_t mods)
{
  if (rec->tok_count >= MACRO_EDITOR_MAX_TOKS) {
    return false;
  }
  for (uint16_t i = rec->tok_count; i > rec->cursor; i--) {
    rec->tokens[i] = rec->tokens[i - 1U];
  }
  rec->tokens[rec->cursor].hid  = hid;
  rec->tokens[rec->cursor].mods = mods;
  rec->tok_count++;
  rec->cursor++;
  rec->edit_dirty = true;
  rec->edit_rev++;
  return true;
}

static bool editor_apply_nav_or_delete(macro_recorder_t *rec, uint8_t code)
{
  if (code == (uint8_t)KC_LEFT) {
    macro_recorder_editor_move(rec, -1);
    return true;
  }
  if (code == (uint8_t)KC_RGHT) {
    macro_recorder_editor_move(rec, 1);
    return true;
  }
  if (code == (uint8_t)KC_UP) {
    macro_recorder_editor_move(rec, -(int16_t)MACRO_EDITOR_COLS);
    return true;
  }
  if (code == (uint8_t)KC_DOWN) {
    macro_recorder_editor_move(rec, (int16_t)MACRO_EDITOR_COLS);
    return true;
  }
  if (code == (uint8_t)KC_HOME) {
    if (rec->cursor != 0U) {
      rec->cursor = 0U;
      rec->edit_rev++;
    }
    return true;
  }
  if (code == (uint8_t)KC_END) {
    if (rec->cursor != rec->tok_count) {
      rec->cursor = rec->tok_count;
      rec->edit_rev++;
    }
    return true;
  }
  if (code == (uint8_t)KC_BSPC) {
    if (rec->cursor > 0U) {
      for (uint16_t i = (uint16_t)(rec->cursor - 1U); (uint16_t)(i + 1U) < rec->tok_count; i++) {
        rec->tokens[i] = rec->tokens[i + 1U];
      }
      rec->tok_count--;
      rec->cursor--;
      rec->edit_dirty = true;
      rec->edit_rev++;
    }
    return true;
  }
  if (code == (uint8_t)KC_DEL) {
    if (rec->cursor < rec->tok_count) {
      for (uint16_t i = rec->cursor; (uint16_t)(i + 1U) < rec->tok_count; i++) {
        rec->tokens[i] = rec->tokens[i + 1U];
      }
      rec->tok_count--;
      rec->edit_dirty = true;
      rec->edit_rev++;
    }
    return true;
  }
  return false;
}

static macro_rec_evt_t step_review_editor(macro_recorder_t *rec, macro_rec_tick_ctx_t const *ctx)
{
  /* Auto-repeat held navigation/delete keys smoothly like a real text editor */
  if (rec->repeat_hid != 0U && (int32_t)(ctx->now_ms - rec->repeat_next_ms) >= 0) {
    rec->repeat_next_ms = ctx->now_ms + EDITOR_REPEAT_STEP_MS;
    (void)editor_apply_nav_or_delete(rec, rec->repeat_hid);
  }

  for (uint8_t r = 0U; r < TK75_KEYMAP_ROWS; r++) {
    for (uint8_t c = 0U; c < HALL_COLS; c++) {
      uint32_t slot = ((uint32_t)r * HALL_COLS) + c;
      bool     now  = hall_engine_is_pressed(ctx->hall, r, c);
      bool     prev = (rec->prev_pressed[slot] != 0U);
      rec->prev_pressed[slot] = now ? 1U : 0U;

      if (now && !prev) {
        if (is_fn_stop_key(ctx->cfg, r, c)) {
          if (rec->edit_dirty) {
            rec->len = macro_recorder_serialize_tokens(rec->tokens, rec->tok_count,
                                                       rec->buf, MACRO_REC_MAX_BYTES);
          }
          return commit_and_exit(rec, ctx);
        }
        uint16_t kc = lookup_record_kc(ctx->cfg, r, c);
        if (!is_recordable_hid(kc)) {
          continue;
        }
        uint8_t code = (uint8_t)kc;
        if (is_mod_hid(code)) {
          uint8_t bit = (uint8_t)(1U << (code - 0xE0U));
          rec->rec_held_mods |= bit;
          rec->mod_used_mask &= (uint8_t)(~bit);
          continue;
        }
        if (rec->rec_held_mods == 0U && editor_apply_nav_or_delete(rec, code)) {
          if (code == (uint8_t)KC_LEFT || code == (uint8_t)KC_RGHT ||
              code == (uint8_t)KC_UP   || code == (uint8_t)KC_DOWN ||
              code == (uint8_t)KC_BSPC || code == (uint8_t)KC_DEL) {
            rec->repeat_hid     = code;
            rec->repeat_next_ms = ctx->now_ms + EDITOR_REPEAT_INIT_MS;
          }
          continue;
        }
        rec->repeat_hid     = 0U;
        rec->mod_used_mask |= rec->rec_held_mods;
        (void)editor_insert_token(rec, code, rec->rec_held_mods);
      } else if (!now && prev) {
        uint16_t kc = lookup_record_kc(ctx->cfg, r, c);
        if (!is_recordable_hid(kc)) {
          continue;
        }
        uint8_t code = (uint8_t)kc;
        if (code == rec->repeat_hid) {
          rec->repeat_hid = 0U;
        }
        if (is_mod_hid(code)) {
          uint8_t bit = (uint8_t)(1U << (code - 0xE0U));
          rec->rec_held_mods &= (uint8_t)(~bit);
          if ((rec->mod_used_mask & bit) == 0U) {
            (void)editor_insert_token(rec, code, 0U);
          }
          rec->mod_used_mask &= (uint8_t)(~bit);
        }
      }
    }
  }
  return MACRO_REC_EVT_NONE;
}

macro_rec_evt_t macro_recorder_tick(macro_recorder_t *rec, macro_rec_tick_ctx_t const *ctx)
{
  if (rec == NULL || ctx == NULL || ctx->hall == NULL || ctx->cfg == NULL) {
    return MACRO_REC_EVT_NONE;
  }

  if (rec->state == MACRO_REC_IDLE) {
    if (!rec->hold_armed) {
      return MACRO_REC_EVT_NONE;
    }
    /* If user released Fn before 1.8s while holding macro key, disarm and play slot */
    if ((ctx->layer_state & (1UL << 1U)) == 0U) {
      rec->hold_armed = false;
      rec->play_slot  = rec->hold_slot;
      return MACRO_REC_EVT_PLAY_SLOT;
    }
    if ((uint32_t)(ctx->now_ms - rec->hold_start_ms) >= MACRO_REC_HOLD_MS) {
      rec->hold_armed    = false;
      rec->rec_slot      = rec->hold_slot;
      rec->len           = 0U;
      rec->rec_held_mods = 0U;
      rec->mod_used_mask = 0U;
      rec->tok_count     = 0U;
      rec->cursor        = 0U;
      rec->edit_dirty    = false;
      rec->repeat_hid    = 0U;
      rec->state         = MACRO_REC_WAIT_START_RELEASE;
      snapshot_pressed(rec, ctx->hall);
      return MACRO_REC_EVT_STARTED;
    }
    return MACRO_REC_EVT_NONE;
  }

  if (rec->state == MACRO_REC_WAIT_START_RELEASE) {
    if (!any_key_pressed(ctx->hall)) {
      memset(rec->prev_pressed, 0, sizeof(rec->prev_pressed));
      rec->state = MACRO_REC_RECORDING;
    }
    return MACRO_REC_EVT_NONE;
  }

  if (rec->state == MACRO_REC_WAIT_REVIEW_RELEASE) {
    if (!any_key_pressed(ctx->hall)) {
      memset(rec->prev_pressed, 0, sizeof(rec->prev_pressed));
      rec->state = MACRO_REC_REVIEW;
    }
    return MACRO_REC_EVT_NONE;
  }

  if (rec->state == MACRO_REC_REVIEW) {
    return step_review_editor(rec, ctx);
  }

  if (rec->state == MACRO_REC_WAIT_STOP_RELEASE) {
    if (!any_key_pressed(ctx->hall)) {
      memset(rec->prev_pressed, 0, sizeof(rec->prev_pressed));
      rec->state = MACRO_REC_IDLE;
    }
    return MACRO_REC_EVT_NONE;
  }

  /* MACRO_REC_RECORDING: scan press/release edges into VIA bytecode */
  for (uint8_t r = 0U; r < TK75_KEYMAP_ROWS; r++) {
    for (uint8_t c = 0U; c < HALL_COLS; c++) {
      uint32_t slot = ((uint32_t)r * HALL_COLS) + c;
      bool     now  = hall_engine_is_pressed(ctx->hall, r, c);
      bool     prev = (rec->prev_pressed[slot] != 0U);
      rec->prev_pressed[slot] = now ? 1U : 0U;

      if (now && !prev) {
        if (is_fn_stop_key(ctx->cfg, r, c)) {
          return finish_recording(rec, ctx);
        }
        uint16_t kc = lookup_record_kc(ctx->cfg, r, c);
        if (is_recordable_hid(kc)) {
          uint8_t code = (uint8_t)kc;
          if (code >= 0xE0U && code <= 0xE7U) {
            rec->rec_held_mods |= (uint8_t)(1U << (code - 0xE0U));
          }
          append_event(rec, SS_DOWN_CODE, code);
        }
      } else if (!now && prev) {
        uint16_t kc = lookup_record_kc(ctx->cfg, r, c);
        if (is_recordable_hid(kc)) {
          uint8_t code = (uint8_t)kc;
          if (code >= 0xE0U && code <= 0xE7U) {
            rec->rec_held_mods &= (uint8_t)(~(1U << (code - 0xE0U)));
          }
          /* Collapse consecutive DOWN(code) + UP(code) into compact VIA SS_TAP(code) */
          if (rec->len >= 3U &&
              rec->buf[rec->len - 3U] == SS_QMK_PREFIX &&
              rec->buf[rec->len - 2U] == SS_DOWN_CODE &&
              rec->buf[rec->len - 1U] == code) {
            rec->buf[rec->len - 2U] = SS_TAP_CODE;
          } else {
            append_event(rec, SS_UP_CODE, code);
          }
        }
      }

      if ((uint16_t)(rec->len + 3U) > MACRO_REC_MAX_BYTES) {
        return finish_recording(rec, ctx);
      }
    }
  }
  return MACRO_REC_EVT_NONE;
}
