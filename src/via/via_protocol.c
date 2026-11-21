#include "via_protocol.h"
#include "default_keymap_iso.h"
#include <string.h>

/* VIA v12 Command IDs */
enum {
  ID_GET_PROTOCOL_VERSION           = 0x01,
  ID_GET_KEYBOARD_VALUE             = 0x02,
  ID_SET_KEYBOARD_VALUE             = 0x03,
  ID_DYNAMIC_KEYMAP_GET_KEYCODE     = 0x04,
  ID_DYNAMIC_KEYMAP_SET_KEYCODE     = 0x05,
  ID_DYNAMIC_KEYMAP_RESET           = 0x06,
  ID_CUSTOM_SET_VALUE               = 0x07,
  ID_CUSTOM_GET_VALUE               = 0x08,
  ID_CUSTOM_SAVE                    = 0x09,
  ID_EEPROM_RESET                   = 0x0A,
  ID_BOOTLOADER_JUMP                = 0x0B,
  ID_DYNAMIC_KEYMAP_MACRO_GET_COUNT = 0x0C,
  ID_DYNAMIC_KEYMAP_MACRO_GET_SIZE  = 0x0D,
  ID_DYNAMIC_KEYMAP_MACRO_GET_BUF   = 0x0E,
  ID_DYNAMIC_KEYMAP_MACRO_SET_BUF   = 0x0F,
  ID_DYNAMIC_KEYMAP_MACRO_RESET     = 0x10,
  ID_DYNAMIC_KEYMAP_GET_LAYER_COUNT = 0x11,
  ID_DYNAMIC_KEYMAP_GET_BUFFER      = 0x12,
  ID_DYNAMIC_KEYMAP_SET_BUFFER      = 0x13,
  ID_DYNAMIC_KEYMAP_GET_ENCODER     = 0x14,
  ID_DYNAMIC_KEYMAP_SET_ENCODER     = 0x15,
  ID_UNHANDLED                      = 0xFF
};

enum {
  VIA_KBD_UPTIME              = 0x01,
  VIA_KBD_LAYOUT_OPTIONS      = 0x02,
  VIA_KBD_SWITCH_MATRIX_STATE = 0x03,
  VIA_KBD_FIRMWARE_VERSION    = 0x04,
};

enum {
  VIA_CH_QMK_RGBLIGHT   = 2,
  VIA_CH_QMK_RGB_MATRIX = 3,
  VIA_CH_TK75_HALL      = 5,
  VIA_CH_TK75_SIDELIGHT = 6,
  VIA_CH_TK75_TFT_ENC   = 7,
  VIA_CH_TK75_EXT       = 8,
  VIA_CH_TK75_WIRELESS  = 9,
};

static uint16_t read_u16_be(uint8_t const *p)
{
  return (uint16_t)(((uint16_t)p[0] << 8U) | p[1]);
}

static void write_u16_be(uint8_t *p, uint16_t v)
{
  p[0] = (uint8_t)((v >> 8U) & 0xFFU);
  p[1] = (uint8_t)(v & 0xFFU);
}

static void write_u32_be(uint8_t *p, uint32_t v)
{
  p[0] = (uint8_t)((v >> 24U) & 0xFFU);
  p[1] = (uint8_t)((v >> 16U) & 0xFFU);
  p[2] = (uint8_t)((v >> 8U) & 0xFFU);
  p[3] = (uint8_t)(v & 0xFFU);
}

static uint16_t clamp_step(uint8_t step, uint8_t lo, uint8_t hi, uint16_t unit_um)
{
  uint8_t s = (step < lo) ? lo : ((step > hi) ? hi : step);
  return (uint16_t)s * unit_um;
}

static void emit_simple_event(via_context_t *via, via_event_type_t type)
{
  if (via->on_event != NULL) {
    via_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = type;
    via->on_event(&evt, via->event_ctx);
  }
}

static void handle_get_keyboard_value(via_context_t *via, uint8_t const *req, uint8_t *rep)
{
  switch (req[1]) {
    case VIA_KBD_UPTIME:
      write_u32_be(&rep[2], via->uptime_ms);
      break;
    case VIA_KBD_LAYOUT_OPTIONS:
      write_u32_be(&rep[2], (uint32_t)via->cfg->layout_options);
      break;
    case VIA_KBD_SWITCH_MATRIX_STATE:
      for (uint8_t r = 0U; r < TK75_KEYMAP_ROWS; r++) {
        uint16_t row_bits = 0U;
        for (uint8_t c = 0U; c < HALL_COLS; c++) {
          if (hall_engine_is_pressed(via->hall, r, c)) {
            row_bits |= (uint16_t)(1U << c);
          }
        }
        write_u16_be(&rep[2U + (r * 2U)], row_bits);
      }
      break;
    case VIA_KBD_FIRMWARE_VERSION:
      write_u32_be(&rep[2], 0x00010000UL);
      break;
    default:
      rep[0] = ID_UNHANDLED;
      break;
  }
}

static void handle_rgb_custom(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  tk75_persist_cfg_t *c = via->cfg;
  uint8_t val_id = req[2];

  if (is_set) {
    switch (val_id) {
      case 1U: c->rgb_brightness = req[3]; c->rgb_enabled = (req[3] > 0U) ? 1U : c->rgb_enabled; break;
      case 2U: c->rgb_effect     = req[3]; c->rgb_enabled = (req[3] > 0U) ? 1U : 0U; break;
      case 3U: c->rgb_speed      = req[3]; break;
      case 4U: c->rgb_hue = req[3]; c->rgb_sat = req[4]; break;
      default: rep[0] = ID_UNHANDLED; return;
    }
    emit_simple_event(via, VIA_EVT_RGB_CHANGED);
    return;
  }

  switch (val_id) {
    case 1U: rep[3] = c->rgb_brightness; break;
    case 2U: rep[3] = (c->rgb_enabled != 0U) ? c->rgb_effect : 0U; break;
    case 3U: rep[3] = c->rgb_speed; break;
    case 4U: rep[3] = c->rgb_hue; rep[4] = c->rgb_sat; break;
    default: rep[0] = ID_UNHANDLED; break;
  }
}

static void handle_hall_custom(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  tk75_persist_cfg_t *c = via->cfg;
  uint8_t val_id = req[2];

  if (is_set) {
    switch (val_id) {
      case 1U: c->hall_actuation_um  = clamp_step(req[3], 2U, 78U, 50U); break;
      case 2U: c->hall_hyst_um       = clamp_step(req[3], 1U, 20U, 50U); break;
      case 3U: c->hall_rt_enabled    = (req[3] != 0U) ? 1U : 0U; break;
      case 4U: c->hall_rt_continuous = (req[3] != 0U) ? 1U : 0U; break;
      case 5U: c->hall_rt_down_um    = clamp_step(req[3], 2U, 40U, 50U); break;
      case 6U: c->hall_rt_up_um      = clamp_step(req[3], 2U, 40U, 50U); break;
      case 7U: c->hall_socd_mode     = (req[3] <= 3U) ? req[3] : 0U; break;
      case 8U: c->hall_socd_preset   = (req[3] <= 2U) ? req[3] : 0U; break;
      case 9U: emit_simple_event(via, VIA_EVT_RECALIBRATE_IDLE); return;
      default: rep[0] = ID_UNHANDLED; return;
    }
    emit_simple_event(via, VIA_EVT_HALL_CONFIG_CHANGED);
    return;
  }

  switch (val_id) {
    case 1U: rep[3] = (uint8_t)(c->hall_actuation_um / 50U); break;
    case 2U: rep[3] = (uint8_t)(c->hall_hyst_um / 50U); break;
    case 3U: rep[3] = c->hall_rt_enabled; break;
    case 4U: rep[3] = c->hall_rt_continuous; break;
    case 5U: rep[3] = (uint8_t)(c->hall_rt_down_um / 50U); break;
    case 6U: rep[3] = (uint8_t)(c->hall_rt_up_um / 50U); break;
    case 7U: rep[3] = c->hall_socd_mode; break;
    case 8U: rep[3] = c->hall_socd_preset; break;
    case 9U: rep[3] = 0U; break; /* Action button: id_tk75_recal_idle */
    default: rep[0] = ID_UNHANDLED; break;
  }
}

static void handle_sidelight_custom(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  tk75_persist_cfg_t *c = via->cfg;
  uint8_t val_id = req[2];

  if (is_set) {
    switch (val_id) {
      case 1U: c->side_enabled     = (req[3] != 0U) ? 1U : 0U; break;
      case 2U: c->side_brightness  = req[3]; break;
      case 3U: c->side_effect      = req[3]; break;
      case 4U: c->side_speed       = req[3]; break;
      case 5U: c->side_hue = req[3]; c->side_sat = req[4]; break;
      case 6U: c->caps_ind_enabled  = (req[3] != 0U) ? 1U : 0U; break;
      case 7U: c->rgb_enabled       = (req[3] != 0U) ? 1U : 0U; break;
      case 8U: c->caps_side_enabled = (req[3] != 0U) ? 1U : 0U; break;
      case 9U: c->caps_side_effect  = (req[3] <= 4U) ? req[3] : 0U; break;
      default: rep[0] = ID_UNHANDLED; return;
    }
    emit_simple_event(via, VIA_EVT_RGB_CHANGED);
    return;
  }

  switch (val_id) {
    case 1U: rep[3] = c->side_enabled; break;
    case 2U: rep[3] = c->side_brightness; break;
    case 3U: rep[3] = c->side_effect; break;
    case 4U: rep[3] = c->side_speed; break;
    case 5U: rep[3] = c->side_hue; rep[4] = c->side_sat; break;
    case 6U: rep[3] = c->caps_ind_enabled; break;
    case 7U: rep[3] = c->rgb_enabled; break;
    case 8U: rep[3] = c->caps_side_enabled; break;
    case 9U: rep[3] = c->caps_side_effect; break;
    default: rep[0] = ID_UNHANDLED; break;
  }
}

static void handle_tft_custom(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  tk75_persist_cfg_t *c = via->cfg;
  uint8_t val_id = req[2];

  if (is_set) {
    switch (val_id) {
      case 1U: c->tft_enabled        = (req[3] != 0U) ? 1U : 0U; break;
      case 2U: c->tft_page           = (req[3] <= 5U) ? req[3] : 0U; break;
      case 3U: c->tft_brightness     = (req[3] <= 100U) ? req[3] : 100U; break;
      case 4U: c->tft_timeout_idx    = (req[3] <= 3U) ? req[3] : 2U; break;
      case 5U: c->tft_clock_24h      = (req[3] != 0U) ? 1U : 0U; break;
      case 6U: c->tft_wpm_enabled    = (req[3] != 0U) ? 1U : 0U; break;
      case 7U: c->tft_he_bar_enabled = (req[3] != 0U) ? 1U : 0U; break;
      case 8U: c->encoder_mode       = (req[3] <= 3U) ? req[3] : 0U; break;
      case 9U: c->tft_screensaver    = (req[3] != 0U) ? 1U : 0U; break;
      default: rep[0] = ID_UNHANDLED; return;
    }
    emit_simple_event(via, VIA_EVT_TFT_CHANGED);
    return;
  }

  switch (val_id) {
    case 1U: rep[3] = c->tft_enabled; break;
    case 2U: rep[3] = c->tft_page; break;
    case 3U: rep[3] = c->tft_brightness; break;
    case 4U: rep[3] = c->tft_timeout_idx; break;
    case 5U: rep[3] = c->tft_clock_24h; break;
    case 6U: rep[3] = c->tft_wpm_enabled; break;
    case 7U: rep[3] = c->tft_he_bar_enabled; break;
    case 8U: rep[3] = c->encoder_mode; break;
    case 9U: rep[3] = c->tft_screensaver; break;
    default: rep[0] = ID_UNHANDLED; break;
  }
}

static void handle_ext_channel(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  uint8_t val_id = req[2];

  if (!is_set && val_id == 1U) {
    uint8_t offset = req[3];
    uint8_t count  = 0U;
    for (uint8_t i = 0U; i < 27U && (uint16_t)offset + i < HALL_SLOT_COUNT; i++) {
      uint8_t slot = (uint8_t)(offset + i);
      uint16_t um = hall_engine_get_travel_um(via->hall, slot / HALL_COLS, slot % HALL_COLS);
      rep[5U + i] = (uint8_t)(um / 100U); /* 0..40 in 0.1 mm steps */
      count++;
    }
    rep[3] = offset;
    rep[4] = count;
    return;
  }

  if (is_set && val_id == 2U && via->on_event != NULL) {
    via_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type      = VIA_EVT_RTC_SYNC;
    evt.rtc.year  = read_u16_be(&req[3]);
    evt.rtc.month = req[5];
    evt.rtc.day   = req[6];
    evt.rtc.hour  = req[7];
    evt.rtc.min   = req[8];
    evt.rtc.sec   = req[9];
    via->on_event(&evt, via->event_ctx);
    return;
  }

  if (is_set && val_id == 3U && via->on_event != NULL) {
    via_event_t evt;
    memset(&evt, 0, sizeof(evt));
    evt.type        = VIA_EVT_TFT_MEDIA_CHUNK;
    evt.media.flags = req[3];
    evt.media.seq   = read_u16_be(&req[4]);
    evt.media.len   = (req[6] <= 24U) ? req[6] : 24U;
    memcpy(evt.media.data, &req[7], evt.media.len);
    via->on_event(&evt, via->event_ctx);
    return;
  }

  if (val_id == 4U && req[3] < HALL_ROWS && req[4] < HALL_COLS) {
    uint32_t slot = ((uint32_t)req[3] * HALL_COLS) + req[4];
    if (is_set) {
      via->cfg->hall_per_key_act_step[slot] = req[5];
      via->cfg->hall_per_key_rt[slot]       = req[6];
      emit_simple_event(via, VIA_EVT_HALL_CONFIG_CHANGED);
    } else {
      rep[5] = via->cfg->hall_per_key_act_step[slot];
      rep[6] = via->cfg->hall_per_key_rt[slot];
    }
    return;
  }

  if (!is_set && val_id == 6U) {
    via_diag_t const *d = via->diag;
    if (d == NULL) {
      rep[0] = ID_UNHANDLED;
      return;
    }
    if (req[3] == 0U) {
      rep[4]  = d->conn_mode;
      rep[5]  = d->bt_slot;
      rep[6]  = d->rf_state;
      rep[7]  = d->battery_pct;
      rep[8]  = d->vbus_present;
      rep[9]  = d->charging;
      rep[10] = d->rf_link_up;
      write_u16_be(&rep[11], d->hall_min_adc);
      write_u16_be(&rep[13], d->hall_max_adc);
      write_u16_be(&rep[15], d->pressed_count);
      rep[17] = d->cal_valid_count;
      rep[18] = d->asleep;
      write_u16_be(&rep[19], d->vbat_adc);
    } else if (req[3] == 1U) {
      write_u16_be(&rep[4], d->tft_rx_count);
      rep[6] = d->tft_last_cmd;
      rep[7] = d->tft_last_len;
      memcpy(&rep[8], d->tft_last_payload, sizeof(d->tft_last_payload));
      write_u16_be(&rep[24], d->tft_menu_cursor);
      rep[26] = d->tft_menu_pic_slot;
      rep[27] = d->enc_raw_bits;
    } else if (req[3] == 2U) {
      rep[4] = d->tft_info_len;
      memcpy(&rep[5], d->tft_info, sizeof(d->tft_info));
    } else {
      rep[0] = ID_UNHANDLED;
    }
    return;
  }

  if (val_id == 7U) {
    if (is_set) {
      if (via->on_event == NULL) {
        rep[0] = ID_UNHANDLED;
        return;
      }
      via_event_t evt;
      memset(&evt, 0, sizeof(evt));
      evt.type       = VIA_EVT_TFT_RAW;
      evt.tft_raw.op = req[3];
      if (req[3] == 0U) {
        evt.tft_raw.len = req[4]; /* 1=pause HUD, 0=resume HUD */
      } else if (req[3] == 1U) {
        evt.tft_raw.offset = req[4];
        evt.tft_raw.len    = (req[5] <= 24U) ? req[5] : 24U;
        memcpy(evt.tft_raw.data, &req[6], evt.tft_raw.len);
      } else if (req[3] == 2U) {
        evt.tft_raw.cmd = req[4];
        evt.tft_raw.len = req[5]; /* if <=24, inline in req[6..29], else staged */
        if (req[5] <= 24U) {
          memcpy(evt.tft_raw.data, &req[6], req[5]);
        }
      } else if (req[3] == 3U) {
        evt.tft_raw.offset = req[4];
        evt.tft_raw.len    = (req[5] <= 24U) ? req[5] : 24U;
        evt.tft_raw.cmd    = req[31];
        memcpy(evt.tft_raw.data, &req[6], evt.tft_raw.len);
      } else {
        rep[0] = ID_UNHANDLED;
        return;
      }
      via->on_event(&evt, via->event_ctx);
      return;
    } else {
      via_diag_t const *d = via->diag;
      if (d == NULL) {
        rep[0] = ID_UNHANDLED;
        return;
      }
      rep[4] = d->tft_raw_rx_ready;
      rep[5] = d->tft_raw_rx_cmd;
      rep[6] = d->tft_raw_rx_len;
      memcpy(&rep[7], d->tft_raw_rx_payload, sizeof(d->tft_raw_rx_payload));
      if (req[3] == 1U && via->on_event != NULL) {
        via_event_t evt;
        memset(&evt, 0, sizeof(evt));
        evt.type       = VIA_EVT_TFT_RAW;
        evt.tft_raw.op = 4U; /* clear rx_ready */
        via->on_event(&evt, via->event_ctx);
      }
      return;
    }
  }

  rep[0] = ID_UNHANDLED;
}

static void handle_wireless_custom(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  uint8_t val_id = req[2];
  uint8_t val    = req[3];

  switch (val_id) {
    case 1U: /* Active Bluetooth Slot: 0=BT1, 1=BT2, 2=BT3 */
      if (is_set) {
        via->cfg->rf_bt_slot = (val < 3U) ? val : 0U;
        emit_simple_event(via, VIA_EVT_RF_CONFIG_CHANGED);
      } else {
        rep[3] = via->cfg->rf_bt_slot;
      }
      return;
    case 2U: /* Wireless Poll Rate: 0=1000Hz, 1=500Hz, 2=250Hz, 3=125Hz */
      if (is_set) {
        via->cfg->rf_poll_rate_idx = (val < 4U) ? val : 0U;
        emit_simple_event(via, VIA_EVT_RF_CONFIG_CHANGED);
      } else {
        rep[3] = via->cfg->rf_poll_rate_idx;
      }
      return;
    case 3U: /* OS Mode: 0=Win/Linux, 1=macOS */
      if (is_set) {
        via->cfg->mac_mode = (val != 0U) ? 1U : 0U;
        emit_simple_event(via, VIA_EVT_RF_CONFIG_CHANGED);
      } else {
        rep[3] = via->cfg->mac_mode;
      }
      return;
    case 4U: /* Battery Idle Sleep Timeout (minutes, 0=never, max 30) */
      if (is_set) {
        via->cfg->sleep_timeout_min = (val <= 30U) ? val : 30U;
        emit_simple_event(via, VIA_EVT_RF_CONFIG_CHANGED);
      } else {
        rep[3] = via->cfg->sleep_timeout_min;
      }
      return;
    case 5U: /* Trigger BLE/2.4G pairing */
      if (is_set && val != 0U) {
        emit_simple_event(via, VIA_EVT_RF_PAIR);
      } else {
        rep[3] = 0U;
      }
      return;
    default:
      rep[0] = ID_UNHANDLED;
      return;
  }
}

static void dispatch_custom_channel(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  switch (req[1]) {
    case VIA_CH_QMK_RGBLIGHT:
    case VIA_CH_QMK_RGB_MATRIX:
      handle_rgb_custom(via, is_set, req, rep);
      break;
    case VIA_CH_TK75_HALL:
      handle_hall_custom(via, is_set, req, rep);
      break;
    case VIA_CH_TK75_SIDELIGHT:
      handle_sidelight_custom(via, is_set, req, rep);
      break;
    case VIA_CH_TK75_TFT_ENC:
      handle_tft_custom(via, is_set, req, rep);
      break;
    case VIA_CH_TK75_EXT:
      handle_ext_channel(via, is_set, req, rep);
      break;
    case VIA_CH_TK75_WIRELESS:
      handle_wireless_custom(via, is_set, req, rep);
      break;
    default:
      rep[0] = ID_UNHANDLED;
      break;
  }
}

static void handle_keymap_buffer(via_context_t *via, bool is_set, uint8_t const *req, uint8_t *rep)
{
  uint16_t offset = read_u16_be(&req[1]);
  uint8_t  size   = req[3];
  uint32_t total_slots = TK75_KEYMAP_LAYERS * TK75_KEYMAP_ROWS * TK75_KEYMAP_COLS;

  if (size > 28U || (offset & 1U) != 0U || (size & 1U) != 0U) {
    rep[0] = ID_UNHANDLED;
    return;
  }

  uint16_t *flat = &via->cfg->keymap[0][0][0];
  for (uint8_t i = 0U; i < size; i += 2U) {
    uint32_t idx = ((uint32_t)offset + i) >> 1U;
    if (idx >= total_slots) {
      break;
    }
    if (is_set) {
      flat[idx] = read_u16_be(&req[4U + i]);
    } else {
      write_u16_be(&rep[4U + i], flat[idx]);
    }
  }
  if (is_set) {
    emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
  }
}

void via_process_packet(via_context_t *via, uint8_t const in_req[VIA_RAW_EPSIZE], uint8_t out_reply[VIA_RAW_EPSIZE])
{
  memcpy(out_reply, in_req, VIA_RAW_EPSIZE);
  if (via == NULL || via->cfg == NULL) {
    out_reply[0] = ID_UNHANDLED;
    return;
  }

  switch (in_req[0]) {
    case ID_GET_PROTOCOL_VERSION:
      write_u16_be(&out_reply[1], VIA_PROTOCOL_VERSION);
      break;

    case ID_GET_KEYBOARD_VALUE:
      handle_get_keyboard_value(via, in_req, out_reply);
      break;

    case ID_SET_KEYBOARD_VALUE:
      if (in_req[1] == VIA_KBD_LAYOUT_OPTIONS) {
        via->cfg->layout_options = (uint16_t)(((uint16_t)in_req[4] << 8U) | in_req[5]);
        emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
      } else {
        out_reply[0] = ID_UNHANDLED;
      }
      break;

    case ID_DYNAMIC_KEYMAP_GET_KEYCODE:
      if (in_req[1] < TK75_KEYMAP_LAYERS && in_req[2] < TK75_KEYMAP_ROWS && in_req[3] < TK75_KEYMAP_COLS) {
        write_u16_be(&out_reply[4], via->cfg->keymap[in_req[1]][in_req[2]][in_req[3]]);
      } else {
        out_reply[0] = ID_UNHANDLED;
      }
      break;

    case ID_DYNAMIC_KEYMAP_SET_KEYCODE:
      if (in_req[1] < TK75_KEYMAP_LAYERS && in_req[2] < TK75_KEYMAP_ROWS && in_req[3] < TK75_KEYMAP_COLS) {
        via->cfg->keymap[in_req[1]][in_req[2]][in_req[3]] = read_u16_be(&in_req[4]);
        emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
      } else {
        out_reply[0] = ID_UNHANDLED;
      }
      break;

    case ID_DYNAMIC_KEYMAP_RESET:
      keymap_copy_default_layers(via->cfg->keymap);
      keymap_copy_default_encoders(via->cfg->encoders);
      emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
      break;

    case ID_CUSTOM_SET_VALUE:
      dispatch_custom_channel(via, true, in_req, out_reply);
      break;

    case ID_CUSTOM_GET_VALUE:
      dispatch_custom_channel(via, false, in_req, out_reply);
      break;

    case ID_CUSTOM_SAVE:
      emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
      break;

    case ID_EEPROM_RESET:
      emit_simple_event(via, VIA_EVT_RESET_EEPROM);
      break;

    case ID_BOOTLOADER_JUMP:
      emit_simple_event(via, VIA_EVT_BOOTLOADER_JUMP);
      break;

    case ID_DYNAMIC_KEYMAP_MACRO_GET_COUNT:
      out_reply[1] = TK75_MACRO_COUNT;
      break;

    case ID_DYNAMIC_KEYMAP_MACRO_GET_SIZE:
      write_u16_be(&out_reply[1], TK75_MACRO_BUF_BYTES);
      break;

    case ID_DYNAMIC_KEYMAP_MACRO_GET_BUF:
    case ID_DYNAMIC_KEYMAP_MACRO_SET_BUF: {
      enum { VIA_MACRO_CHUNK_MAX = 28U };
      uint16_t const off = read_u16_be(&in_req[1]);
      uint8_t  const sz  = in_req[3];
      /*
       * Web VIA polls the macro buffer in fixed 28-byte slices; clamp the final
       * partial slice when TK75_MACRO_BUF_BYTES is not divisible by 28.
       */
      if (sz > VIA_MACRO_CHUNK_MAX || (uint32_t)off >= TK75_MACRO_BUF_BYTES) {
        out_reply[0] = ID_UNHANDLED;
        break;
      }
      uint32_t const rem   = (uint32_t)TK75_MACRO_BUF_BYTES - (uint32_t)off;
      uint8_t  const avail = (sz <= rem) ? sz : (uint8_t)rem;
      if (in_req[0] == ID_DYNAMIC_KEYMAP_MACRO_SET_BUF) {
        memcpy(&via->cfg->macros[off], &in_req[4], avail);
        emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
      } else {
        memset(&out_reply[4], 0, VIA_MACRO_CHUNK_MAX);
        memcpy(&out_reply[4], &via->cfg->macros[off], avail);
      }
      break;
    }

    case ID_DYNAMIC_KEYMAP_MACRO_RESET:
      memset(via->cfg->macros, 0, sizeof(via->cfg->macros));
      emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
      break;

    case ID_DYNAMIC_KEYMAP_GET_LAYER_COUNT:
      out_reply[1] = TK75_KEYMAP_LAYERS;
      break;

    case ID_DYNAMIC_KEYMAP_GET_BUFFER:
      handle_keymap_buffer(via, false, in_req, out_reply);
      break;

    case ID_DYNAMIC_KEYMAP_SET_BUFFER:
      handle_keymap_buffer(via, true, in_req, out_reply);
      break;

    case ID_DYNAMIC_KEYMAP_GET_ENCODER:
      if (in_req[1] < TK75_KEYMAP_LAYERS && in_req[2] == 0U && in_req[3] <= 1U) {
        write_u16_be(&out_reply[4], via->cfg->encoders[in_req[1]][in_req[3]]);
      } else {
        out_reply[0] = ID_UNHANDLED;
      }
      break;

    case ID_DYNAMIC_KEYMAP_SET_ENCODER:
      if (in_req[1] < TK75_KEYMAP_LAYERS && in_req[2] == 0U && in_req[3] <= 1U) {
        via->cfg->encoders[in_req[1]][in_req[3]] = read_u16_be(&in_req[4]);
        emit_simple_event(via, VIA_EVT_SAVE_EEPROM);
      } else {
        out_reply[0] = ID_UNHANDLED;
      }
      break;

    default:
      out_reply[0] = ID_UNHANDLED;
      break;
  }
}
