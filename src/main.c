#include "adc_scanner_at32.h"
#include "board_tk75.h"
#include "encoder.h"
#include "factory_cal.h"
#include "flash_eeprom.h"
#include "hall_cal.h"
#include "hall_engine.h"
#include "hud_engine.h"
#include "keymap.h"
#include "macro_player.h"
#include "macro_recorder.h"
#include "mode_switches.h"
#include "pixel_hud.h"
#include "spi3_rf.h"
#include "rgb_effects.h"
#include "tk75_pins.h"
#include "tusb.h"
#include "uart7_tft.h"
#include "usb_hid.h"
#include "via_protocol.h"
#include "ws2812.h"
#include <string.h>

#define EEPROM_SAVE_DEBOUNCE_MS 1000U
#define RGB_REFRESH_PERIOD_MS   16U
#define ENCODER_POLL_PERIOD_MS  1U
#define HUD_POLL_PERIOD_MS      5U
#define BOOT_JUMP_DELAY_MS      20U
#define RGB_FX_COUNT            7U
#define TFT_PAGE_COUNT          6U
#define ENC_MODE_COUNT          4U

typedef struct {
  hall_pos_t a;
  hall_pos_t b;
  uint8_t    min_preset;
} socd_preset_def_t;

typedef struct {
  tk75_persist_cfg_t  cfg;
  uint8_t             eeprom_slot;
  hall_engine_t       hall;
  hall_cal_t          hall_cal;
  keymap_engine_t     keymap;
  macro_player_t      macro;
  macro_recorder_t    macro_rec;
  rgb_effects_state_t rgb_fx;
  rgb_grb_t           leds[RGB_LED_COUNT];
  via_context_t       via;
  via_diag_t          diag;
  hud_engine_t        hud;
  pixel_hud_t         px_hud;

  keymap_report_t     sent_kbd;
  uint16_t            sent_consumer;
  uint16_t            pending_consumer;
  uint8_t             sent_system;
  bool                prev_boot_proto;
  bool                has_sent_kbd;
  uint16_t            prev_pressed_count;

  uint8_t             via_reply[VIA_RAW_EPSIZE];
  bool                via_reply_pending;

  bool                eeprom_dirty;
  uint32_t            eeprom_dirty_ms;
  bool                boot_jump_pending;
  uint32_t            boot_jump_ms;
  uint32_t            macro_spd_toast_until_ms;
  uint16_t            peak_wpm;
  uint32_t            session_keys;

  uint32_t            last_rgb_ms;
  uint32_t            last_enc_ms;
  uint32_t            last_hud_ms;
  uint32_t            last_batt_ms;
  uint32_t            last_activity_ms;
  uint32_t            tft_power_on_ms;

  uint8_t             active_conn_mode;
  uint8_t             prev_host_leds;
  uint8_t             battery_pct;
  tk75_battery_gauge_t batt_gauge;
  bool                vbus_present;
  bool                charging;
  bool                hw_charging;
  bool                asleep;
  bool                tft_rail_on;
  bool                rf_nkro_active;
  bool                enc_prev_btn;

  bool                tft_raw_paused;
  uint8_t             tft_raw_tx_buf[UART7_MAX_PAYLOAD];
  uint8_t             tft_raw_rx_ready;
  uint8_t             tft_raw_rx_cmd;
  uint8_t             tft_raw_rx_len;
  uint8_t             tft_raw_rx_payload[24];
} tk75_fw_t;

static tk75_fw_t g_fw;

static uint8_t rf_mode_for_conn(uint8_t conn_mode, uint8_t bt_slot)
{
  if (conn_mode == TK75_CONN_BT) {
    return (bt_slot < 3U) ? bt_slot : SPI3_RF_MODE_BT1;
  }
  if (conn_mode == TK75_CONN_24G) {
    return SPI3_RF_MODE_24G;
  }
  return SPI3_RF_MODE_USB;
}

static uint8_t active_host_leds(tk75_fw_t const *fw)
{
  if (fw->active_conn_mode == TK75_CONN_USB) {
    return usb_hid_get_host_leds();
  }
  return spi3_rf_get_state()->host_leds;
}

static bool send_via_reply(tk75_fw_t *fw)
{
  if (fw->active_conn_mode == TK75_CONN_USB) {
    return usb_hid_send_via(fw->via_reply);
  }
  return spi3_rf_send_via(fw->via_reply);
}

static void sync_power_rails(tk75_fw_t *fw)
{
  bool caps_side_on = (fw->cfg.caps_side_enabled != 0U) && ((active_host_leds(fw) & 0x02U) != 0U);
  bool rgb_on       = !fw->asleep && (fw->cfg.rgb_enabled != 0U || fw->cfg.side_enabled != 0U || caps_side_on);
  bool tft_on       = !fw->asleep && (fw->cfg.tft_enabled != 0U);
  board_set_rgb_power(rgb_on);

  if (!tft_on) {
    /* Quiesce UART7 TX/RX to 0V before cutting TFT rail so U24 & ST7735S do a true 0V cold POR */
    uart7_tft_set_bus_power(false);
    board_set_tft_power(false);
    fw->tft_rail_on = false;
    return;
  }

  board_set_tft_power(true);
  if (!fw->tft_rail_on) {
    uint32_t now_ms = board_millis();
    fw->tft_power_on_ms = now_ms;
    board_delay_us(2000U);
    uart7_tft_set_bus_power(true);
    if (fw->px_hud.send_fn != NULL) {
      /* Perform an exact cold-boot Pixel HUD reset whenever TFT rail powers up (Fn+F4 / VIA / wake) */
      hud_send_frame_fn_t sfn  = fw->px_hud.send_fn;
      void               *sctx = fw->px_hud.send_ctx;
      pixel_hud_init(&fw->px_hud, &fw->cfg, sfn, sctx);
      fw->px_hud.last_probe_ms = now_ms;
      fw->cfg.encoder_mode     = 0U;
    }
    hud_engine_mark_options_dirty(&fw->hud);
  }
  fw->tft_rail_on = true;
}

static void enter_battery_sleep(tk75_fw_t *fw)
{
  if (fw->asleep) {
    return;
  }
  fw->asleep = true;
  sync_power_rails(fw);
  board_set_scan_rate_sleep(true);
  spi3_rf_enter_sleep(fw->battery_pct);
}

static void wake_from_sleep(tk75_fw_t *fw, uint32_t now_ms)
{
  fw->last_activity_ms = now_ms;
  if (!fw->asleep) {
    return;
  }
  fw->asleep = false;
  board_set_scan_rate_sleep(false);
  sync_power_rails(fw);
  spi3_rf_wake();
  hud_engine_mark_options_dirty(&fw->hud);
}

static void apply_transport_mode(tk75_fw_t *fw, uint8_t new_conn_mode)
{
  if (new_conn_mode == fw->active_conn_mode) {
    return;
  }

  /* Release any held keys cleanly on the outgoing transport */
  if (fw->active_conn_mode == TK75_CONN_USB) {
    uint8_t zero6[6]   = { 0 };
    uint8_t zero16[16] = { 0 };
    if (fw->prev_boot_proto) {
      usb_hid_send_boot_kbd(0U, zero6);
    } else {
      usb_hid_send_nkro(0U, zero16);
    }
    usb_hid_send_consumer(0U);
    usb_hid_send_system(0U);
  } else {
    uint8_t zero6[6]   = { 0 };
    uint8_t zero15[15] = { 0 };
    if (fw->rf_nkro_active) {
      spi3_rf_send_nkro_kbd(zero15);
      fw->rf_nkro_active = false;
    }
    spi3_rf_send_boot_kbd(0U, zero6);
    spi3_rf_send_consumer(0U);
    spi3_rf_send_system(0U);
  }

  fw->active_conn_mode = new_conn_mode;
  memset(&fw->sent_kbd, 0, sizeof(fw->sent_kbd));
  fw->has_sent_kbd     = false;
  fw->sent_consumer    = 0U;
  fw->pending_consumer = 0U;
  fw->sent_system      = 0U;

  spi3_rf_set_target_mode(rf_mode_for_conn(new_conn_mode, fw->cfg.rf_bt_slot));
  if (new_conn_mode == TK75_CONN_USB) {
    board_usb_set_connect(true);
    tud_connect();
  } else {
    tud_disconnect();
    board_usb_set_connect(false);
  }
}

static socd_preset_def_t const k_socd_defs[3] = {
  { { 3U, 1U }, { 3U, 3U  }, 0U }, /* A + D */
  { { 2U, 2U }, { 3U, 2U  }, 1U }, /* W + S */
  { { 5U, 12U }, { 5U, 14U }, 2U }, /* Left + Right */
};

static uint8_t clamp_u8_step(int32_t v, uint8_t lo, uint8_t hi)
{
  if (v < (int32_t)lo) {
    return lo;
  }
  if (v > (int32_t)hi) {
    return hi;
  }
  return (uint8_t)v;
}

static uint16_t clamp_u16_range(int32_t v, uint16_t lo, uint16_t hi)
{
  if (v < (int32_t)lo) {
    return lo;
  }
  if (v > (int32_t)hi) {
    return hi;
  }
  return (uint16_t)v;
}

static void mark_eeprom_dirty(tk75_fw_t *fw, uint32_t now_ms)
{
  fw->eeprom_dirty    = true;
  fw->eeprom_dirty_ms = now_ms;
}

static void apply_hall_config(tk75_fw_t *fw)
{
  tk75_persist_cfg_t const *c = &fw->cfg;
  hall_key_config_t global_cfg = {
    .actuation_um    = c->hall_actuation_um,
    .release_hyst_um = c->hall_hyst_um,
    .rt_enabled      = (c->hall_rt_enabled != 0U),
    .rt_continuous   = (c->hall_rt_continuous != 0U),
    .rt_down_um      = c->hall_rt_down_um,
    .rt_up_um        = c->hall_rt_up_um,
  };
  hall_engine_set_global_config(&fw->hall, &global_cfg);

  for (uint8_t slot = 0U; slot < HALL_SLOT_COUNT; slot++) {
    uint8_t act_step = c->hall_per_key_act_step[slot];
    uint8_t rt_byte  = c->hall_per_key_rt[slot];
    if (act_step == 0U && (rt_byte & 0x01U) == 0U) {
      continue;
    }
    hall_key_config_t per_key = global_cfg;
    if (act_step >= 2U && act_step <= 78U) {
      per_key.actuation_um = (uint16_t)act_step * 50U;
    }
    if ((rt_byte & 0x01U) != 0U) {
      uint8_t rt_step = (uint8_t)(rt_byte >> 2U);
      per_key.rt_enabled = ((rt_byte & 0x02U) != 0U);
      if (rt_step >= 2U && rt_step <= 40U) {
        per_key.rt_down_um = (uint16_t)rt_step * 50U;
        per_key.rt_up_um   = (uint16_t)rt_step * 50U;
      }
    }
    hall_pos_t pos = { (uint8_t)(slot / HALL_COLS), (uint8_t)(slot % HALL_COLS) };
    hall_engine_set_key_config(&fw->hall, pos, &per_key);
  }

  for (uint8_t i = 0U; i < HALL_SOCD_MAX_PAIRS; i++) {
    hall_socd_pair_t pair;
    memset(&pair, 0, sizeof(pair));
    if (i < 3U && c->hall_socd_mode != HALL_SOCD_OFF && c->hall_socd_preset >= k_socd_defs[i].min_preset) {
      pair.key_a = k_socd_defs[i].a;
      pair.key_b = k_socd_defs[i].b;
      pair.mode  = (hall_socd_mode_t)c->hall_socd_mode;
    }
    hall_engine_set_socd_pair(&fw->hall, i, &pair);
  }
}

static bool hud_uart7_tx_adapter(uart7_tx_frame_t const *frame, void *ctx)
{
  (void)ctx;
  return uart7_tft_send_frame(frame);
}

static bool handle_rgb_keycode(tk75_fw_t *fw, uint16_t kc)
{
  tk75_persist_cfg_t *c = &fw->cfg;
  switch (kc) {
    case RGB_TOG:
      c->rgb_enabled ^= 1U;
      return true;
    case RGB_MOD:
      c->rgb_enabled = 1U;
      c->rgb_effect  = (uint8_t)((c->rgb_effect + 1U) % RGB_FX_COUNT);
      return true;
    case RGB_RMOD:
      c->rgb_enabled = 1U;
      c->rgb_effect  = (c->rgb_effect == 0U) ? (uint8_t)(RGB_FX_COUNT - 1U) : (uint8_t)(c->rgb_effect - 1U);
      return true;
    case RGB_VAI:
      c->rgb_brightness = clamp_u8_step((int32_t)c->rgb_brightness + 16, 0U, 255U);
      return true;
    case RGB_VAD:
      c->rgb_brightness = clamp_u8_step((int32_t)c->rgb_brightness - 16, 0U, 255U);
      return true;
    case RGB_SPI:
      c->rgb_speed = clamp_u8_step((int32_t)c->rgb_speed + 24, 0U, 255U);
      return true;
    case RGB_SPD:
      c->rgb_speed = clamp_u8_step((int32_t)c->rgb_speed - 24, 0U, 255U);
      return true;
    default:
      return false;
  }
}

static void toggle_encoder_tft_mode(tk75_fw_t *fw, uint32_t now_ms)
{
  tk75_persist_cfg_t *c = &fw->cfg;
  if (fw->px_hud.detected) {
    if (c->encoder_mode == 0U) {
      c->encoder_mode = 1U;
      pixel_hud_open_menu(&fw->px_hud, now_ms);
    } else {
      c->encoder_mode = 0U;
      pixel_hud_close_menu(&fw->px_hud);
    }
    keymap_reset_encoder_state(&fw->keymap);
    return;
  }
  if (c->encoder_mode == 0U) {
    c->encoder_mode = 1U;
    if (!hud_engine_is_menu_open(&fw->hud)) {
      (void)hud_engine_menu_input(&fw->hud, HUD_MENU_ACT_OK, now_ms);
    }
  } else {
    /* One Fn+Knob push from any alternate mode immediately restores standard Volume control. */
    c->encoder_mode = 0U;
    hud_engine_close_menu(&fw->hud);
  }
  keymap_reset_encoder_state(&fw->keymap);
  hud_engine_mark_options_dirty(&fw->hud);
}

static bool handle_tk75_keycode(tk75_fw_t *fw, uint16_t kc)
{
  tk75_persist_cfg_t *c = &fw->cfg;
  switch (kc) {
    case KC_TFT_PREV:
    case KC_TFT_NEXT:
    case KC_TFT_OK: {
      hud_menu_act_t act = (kc == KC_TFT_PREV) ? HUD_MENU_ACT_PREV
                         : (kc == KC_TFT_NEXT) ? HUD_MENU_ACT_NEXT
                                               : HUD_MENU_ACT_OK;
      if (pixel_hud_is_active(&fw->px_hud)) {
        bool was_open    = pixel_hud_is_menu_open(&fw->px_hud);
        bool cfg_changed = pixel_hud_input(&fw->px_hud, act, board_millis());
        bool is_open     = pixel_hud_is_menu_open(&fw->px_hud);
        if (kc == KC_TFT_OK || (!was_open && is_open) || (was_open && !is_open)) {
          c->encoder_mode = is_open ? 1U : 0U;
          keymap_reset_encoder_state(&fw->keymap);
        }
        if (!pixel_hud_is_active(&fw->px_hud)) {
          fw->hud.status_dirty  = true;
          fw->hud.menu_dirty    = true;
          fw->hud.options_dirty = true;
        }
        if (cfg_changed) {
          apply_hall_config(fw);
          sync_power_rails(fw);
          keymap_set_mac_mode(&fw->keymap, c->mac_mode != 0U);
          macro_player_set_speed(&fw->macro, c->macro_speed_idx);
        }
        return cfg_changed;
      }
      bool was_open = hud_engine_is_menu_open(&fw->hud);
      bool cfg_changed = hud_engine_menu_input(&fw->hud, act, board_millis());
      bool is_open = hud_engine_is_menu_open(&fw->hud);
      if (kc == KC_TFT_OK || (!was_open && is_open) || (was_open && !is_open)) {
        c->encoder_mode = is_open ? 1U : 0U;
        keymap_reset_encoder_state(&fw->keymap);
        hud_engine_mark_options_dirty(&fw->hud);
      }
      if (cfg_changed) {
        keymap_set_mac_mode(&fw->keymap, c->mac_mode != 0U);
      }
      return cfg_changed;
    }
    case KC_TFT_TOG:
      c->tft_enabled ^= 1U;
      hud_engine_mark_options_dirty(&fw->hud);
      return true;
    case KC_SOCD_CYCLE:
      c->hall_socd_mode = (uint8_t)((c->hall_socd_mode + 1U) % 4U);
      apply_hall_config(fw);
      return true;
    case KC_RT_TOG:
      c->hall_rt_enabled ^= 1U;
      apply_hall_config(fw);
      return true;
    case KC_ACT_UP:
      c->hall_actuation_um = clamp_u16_range((int32_t)c->hall_actuation_um + 100, 100U, 3900U);
      apply_hall_config(fw);
      return true;
    case KC_ACT_DN:
      c->hall_actuation_um = clamp_u16_range((int32_t)c->hall_actuation_um - 100, 100U, 3900U);
      apply_hall_config(fw);
      return true;
    case KC_WIN_LOCK:
      c->win_lock ^= 1U;
      hud_engine_mark_options_dirty(&fw->hud);
      return true;
    case KC_BT1:
    case KC_BT2:
    case KC_BT3:
      c->rf_bt_slot = (uint8_t)(kc - KC_BT1);
      if (fw->active_conn_mode == TK75_CONN_BT) {
        spi3_rf_set_target_mode(c->rf_bt_slot);
      }
      hud_engine_mark_options_dirty(&fw->hud);
      return true;
    case KC_RF_24G:
      if (fw->active_conn_mode == TK75_CONN_24G) {
        spi3_rf_set_target_mode(SPI3_RF_MODE_24G);
      }
      return true;
    case KC_RF_PAIR:
      if (fw->active_conn_mode != TK75_CONN_USB) {
        spi3_rf_request_pair();
      }
      return true;
    case KC_MAC_TOG:
      c->mac_mode = (c->mac_mode == 0U) ? 1U : 0U;
      keymap_set_mac_mode(&fw->keymap, c->mac_mode != 0U);
      hud_engine_mark_options_dirty(&fw->hud);
      return true;
    case KC_MACRO_SPD_DN: {
      uint8_t spd = macro_player_normalize_speed(c->macro_speed_idx);
      if (spd < (uint8_t)MACRO_SPEED_SAFE) {
        spd++;
      }
      c->macro_speed_idx = spd;
      macro_player_set_speed(&fw->macro, spd);
      fw->macro_spd_toast_until_ms = board_millis() + 1500U;
      return true;
    }
    case KC_MACRO_SPD_UP: {
      uint8_t spd = macro_player_normalize_speed(c->macro_speed_idx);
      if (spd > (uint8_t)MACRO_SPEED_FAST) {
        spd--;
      }
      c->macro_speed_idx = spd;
      macro_player_set_speed(&fw->macro, spd);
      fw->macro_spd_toast_until_ms = board_millis() + 1500U;
      return true;
    }
    default:
      return false;
  }
}

static void on_keymap_action(uint16_t keycode, bool pressed, void *ctx)
{
  if (ctx == NULL) {
    return;
  }
  tk75_fw_t *fw  = (tk75_fw_t *)ctx;
  uint32_t   now = board_millis();

  if ((keycode & 0xFFF0U) == QK_MACRO_BASE) {
    macro_key_event_t ev = {
      .macro_id  = (uint8_t)(keycode & 0x0FU),
      .pressed   = pressed,
      .fn_active = ((fw->keymap.layer_state & (1UL << 1U)) != 0U),
      .now_ms    = now,
    };
    if (macro_recorder_on_macro_key(&fw->macro_rec, &ev)) {
      macro_player_trigger(&fw->macro, fw->cfg.macros, ev.macro_id, now);
    }
    return;
  }

  if (!pressed) {
    return;
  }
  fw->last_activity_ms = now;

  if (keycode == QK_BOOT) {
    fw->boot_jump_pending = true;
    fw->boot_jump_ms      = now;
    return;
  }

  if (keycode == KC_ENC_MODE_NEXT) {
    toggle_encoder_tft_mode(fw, now);
    fw->hud.status_dirty = true;
    return;
  }

  if (handle_rgb_keycode(fw, keycode) || handle_tk75_keycode(fw, keycode)) {
    sync_power_rails(fw);
    fw->hud.status_dirty = true;
    mark_eeprom_dirty(fw, now);
  }
}

static void on_via_event(via_event_t const *evt, void *ctx)
{
  if (evt == NULL || ctx == NULL) {
    return;
  }
  tk75_fw_t *fw  = (tk75_fw_t *)ctx;
  uint32_t   now = board_millis();

  switch (evt->type) {
    case VIA_EVT_SAVE_EEPROM:
      mark_eeprom_dirty(fw, now);
      break;
    case VIA_EVT_HALL_CONFIG_CHANGED:
      apply_hall_config(fw);
      fw->hud.status_dirty = true;
      pixel_hud_mark_config_dirty(&fw->px_hud);
      mark_eeprom_dirty(fw, now);
      break;
    case VIA_EVT_RGB_CHANGED:
      sync_power_rails(fw);
      fw->hud.status_dirty = true;
      pixel_hud_mark_config_dirty(&fw->px_hud);
      mark_eeprom_dirty(fw, now);
      break;
    case VIA_EVT_TFT_CHANGED:
      if (fw->px_hud.detected) {
        if (fw->cfg.encoder_mode == 1U) {
          if (!pixel_hud_is_menu_open(&fw->px_hud)) {
            pixel_hud_open_menu(&fw->px_hud, now);
          }
        } else {
          pixel_hud_close_menu(&fw->px_hud);
        }
        pixel_hud_mark_config_dirty(&fw->px_hud);
      } else if (fw->cfg.encoder_mode == 1U) {
        if (!hud_engine_is_menu_open(&fw->hud)) {
          (void)hud_engine_menu_input(&fw->hud, HUD_MENU_ACT_OK, now);
        }
      } else {
        hud_engine_close_menu(&fw->hud);
      }
      if (fw->cfg.encoder_mode != 0U) {
        keymap_reset_encoder_state(&fw->keymap);
      }
      sync_power_rails(fw);
      hud_engine_mark_options_dirty(&fw->hud);
      mark_eeprom_dirty(fw, now);
      break;
    case VIA_EVT_RESET_EEPROM:
      flash_eeprom_erase_all(flash_eeprom_at32_backend());
      tk75_cfg_set_defaults(&fw->cfg);
      flash_eeprom_save(flash_eeprom_at32_backend(), &fw->cfg, &fw->eeprom_slot);
      apply_hall_config(fw);
      sync_power_rails(fw);
      hud_engine_mark_options_dirty(&fw->hud);
      fw->eeprom_dirty = false;
      break;
    case VIA_EVT_BOOTLOADER_JUMP:
      fw->boot_jump_pending = true;
      fw->boot_jump_ms      = now;
      break;
    case VIA_EVT_RECALIBRATE_IDLE:
      hall_engine_seed_idle_baseline(&fw->hall, adc_scanner_capture_blocking());
      break;
    case VIA_EVT_RTC_SYNC:
      hud_engine_set_rtc(&fw->hud, &evt->rtc);
      break;
    case VIA_EVT_TFT_MEDIA_CHUNK:
      hud_engine_push_media_chunk(&fw->hud, &evt->media);
      break;

    case VIA_EVT_RF_CONFIG_CHANGED:
      keymap_set_mac_mode(&fw->keymap, fw->cfg.mac_mode != 0U);
      spi3_rf_set_poll_rate(fw->cfg.rf_poll_rate_idx);
      if (fw->active_conn_mode == TK75_CONN_BT) {
        spi3_rf_set_target_mode(fw->cfg.rf_bt_slot);
      }
      hud_engine_mark_options_dirty(&fw->hud);
      mark_eeprom_dirty(fw, now);
      break;

    case VIA_EVT_RF_PAIR:
      if (fw->active_conn_mode != TK75_CONN_USB) {
        spi3_rf_request_pair();
      }
      break;

    case VIA_EVT_TFT_RAW: {
      via_tft_raw_t const *r = &evt->tft_raw;
      if (r->op == 0U) {
        bool was_paused      = fw->tft_raw_paused;
        fw->tft_raw_paused   = (r->len != 0U);
        fw->tft_raw_rx_ready = 0U;
        if (was_paused && !fw->tft_raw_paused && pixel_hud_is_active(&fw->px_hud)) {
          fw->px_hud.full_redraw_needed = true;
        }
      } else if (r->op == 1U) {
        if ((uint16_t)r->offset + r->len <= UART7_MAX_PAYLOAD) {
          memcpy(&fw->tft_raw_tx_buf[r->offset], r->data, r->len);
        }
      } else if (r->op == 2U || r->op == 3U) {
        uint16_t total_len = r->len;
        uint8_t const *src = fw->tft_raw_tx_buf;
        if (r->op == 3U) {
          if ((uint16_t)r->offset + r->len <= UART7_MAX_PAYLOAD) {
            memcpy(&fw->tft_raw_tx_buf[r->offset], r->data, r->len);
            total_len = (uint16_t)r->offset + r->len;
          } else {
            break;
          }
        } else if (r->len <= 24U) {
          src = r->data;
        }
        if (total_len <= UART7_MAX_PAYLOAD) {
          uint32_t t0 = board_millis();
          while (uart7_tft_tx_busy() && (uint32_t)(board_millis() - t0) < 10U) {}
          fw->tft_raw_rx_ready = 0U;
          uart7_tx_frame_t f   = { .cmd = r->cmd, .payload_len = total_len, .payload = src };
          (void)uart7_tft_send_frame(&f);
        }
      } else if (r->op == 4U) {
        fw->tft_raw_rx_ready = 0U;
      }
      break;
    }

    default:
      break;
  }
}

static void refresh_via_diag(tk75_fw_t *fw)
{
  spi3_rf_state_t const *rf = spi3_rf_get_state();
  via_diag_t *d             = &fw->diag;
  uint16_t min_adc          = 0xFFFFU;
  uint16_t max_adc          = 0U;
  uint16_t pressed          = 0U;
  uint8_t  cal_cnt          = 0U;

  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (fw->hall.active_mask[i] == 0U) {
      continue;
    }
    uint16_t adc = fw->hall.states[i].filtered_adc;
    if (adc < min_adc) {
      min_adc = adc;
    }
    if (adc > max_adc) {
      max_adc = adc;
    }
    if (fw->hall.states[i].logical_pressed) {
      pressed++;
    }
    if (fw->hall.states[i].cal_valid) {
      cal_cnt++;
    }
  }

  d->conn_mode       = fw->active_conn_mode;
  d->bt_slot         = fw->cfg.rf_bt_slot;
  d->rf_state        = (rf != NULL) ? rf->link_state : 0U;
  d->rf_link_up      = (rf != NULL && rf->link_up) ? 1U : 0U;
  d->battery_pct     = fw->battery_pct;
  d->vbus_present    = fw->vbus_present ? 1U : 0U;
  d->charging        = fw->charging ? 1U : 0U;
  d->asleep          = fw->asleep ? 1U : 0U;
  d->tft_rx_count    = fw->hud.rx_count;
  d->tft_last_cmd    = fw->hud.last_rx_cmd;
  d->tft_last_len    = fw->hud.last_rx_len;
  memcpy(d->tft_last_payload, fw->hud.last_rx_payload, sizeof(d->tft_last_payload));
  d->hall_min_adc      = (min_adc == 0xFFFFU) ? 0U : min_adc;
  d->hall_max_adc      = max_adc;
  d->pressed_count     = pressed;
  d->cal_valid_count   = cal_cnt;
  d->vbat_adc          = adc_scanner_get_vbat_raw();
  d->tft_menu_cursor   = fw->hud.menu_cursor;
  d->tft_menu_pic_slot = fw->hud.menu_pic_slot;
  d->enc_raw_bits      = encoder_hw_diag_bits();
  d->tft_info_len      = fw->hud.info_len;
  memcpy(d->tft_info, fw->hud.info_payload, sizeof(d->tft_info));
  d->tft_raw_rx_ready  = fw->tft_raw_rx_ready;
  d->tft_raw_rx_cmd    = fw->tft_raw_rx_cmd;
  d->tft_raw_rx_len    = fw->tft_raw_rx_len;
  memcpy(d->tft_raw_rx_payload, fw->tft_raw_rx_payload, sizeof(d->tft_raw_rx_payload));
}

static void on_via_rx_packet(uint8_t const *packet, uint16_t len)
{
  uint8_t req[VIA_RAW_EPSIZE];
  memset(req, 0, sizeof(req));
  uint16_t copy_len = (len > VIA_RAW_EPSIZE) ? VIA_RAW_EPSIZE : len;
  memcpy(req, packet, copy_len);

  refresh_via_diag(&g_fw);
  g_fw.via.uptime_ms = board_millis();
  via_process_packet(&g_fw.via, req, g_fw.via_reply);
  g_fw.via_reply_pending = (g_fw.active_conn_mode == TK75_CONN_USB)
                             ? !usb_hid_send_via(g_fw.via_reply)
                             : !spi3_rf_send_via(g_fw.via_reply);
}

static void route_encoder_to_hud(tk75_fw_t *fw, encoder_events_t const *ev, bool btn_edge, uint32_t now_ms)
{
  if (pixel_hud_is_active(&fw->px_hud)) {
    bool was_open = pixel_hud_is_menu_open(&fw->px_hud);
    bool changed  = false;
    if (ev->delta != 0) {
      int32_t        steps = (ev->delta > 0) ? ev->delta : -ev->delta;
      hud_menu_act_t act   = (ev->delta > 0) ? HUD_MENU_ACT_NEXT : HUD_MENU_ACT_PREV;
      for (int32_t i = 0; i < steps; i++) {
        if (pixel_hud_input(&fw->px_hud, act, now_ms)) {
          changed = true;
        }
      }
    }
    if (btn_edge) {
      if (pixel_hud_input(&fw->px_hud, HUD_MENU_ACT_OK, now_ms)) {
        changed = true;
      }
    }
    if (was_open && !pixel_hud_is_menu_open(&fw->px_hud) && fw->cfg.encoder_mode == 1U) {
      fw->cfg.encoder_mode = 0U;
    }
    if (!pixel_hud_is_active(&fw->px_hud)) {
      fw->hud.status_dirty  = true;
      fw->hud.menu_dirty    = true;
      fw->hud.options_dirty = true;
    }
    if (changed) {
      apply_hall_config(fw);
      sync_power_rails(fw);
      keymap_set_mac_mode(&fw->keymap, fw->cfg.mac_mode != 0U);
      macro_player_set_speed(&fw->macro, fw->cfg.macro_speed_idx);
      mark_eeprom_dirty(fw, now_ms);
    }
    return;
  }

  bool was_open = hud_engine_is_menu_open(&fw->hud);
  bool changed  = false;
  if (ev->delta != 0) {
    int32_t        steps = (ev->delta > 0) ? ev->delta : -ev->delta;
    hud_menu_act_t act   = (ev->delta > 0) ? HUD_MENU_ACT_NEXT : HUD_MENU_ACT_PREV;
    for (int32_t i = 0; i < steps; i++) {
      if (hud_engine_menu_input(&fw->hud, act, now_ms)) {
        keymap_set_mac_mode(&fw->keymap, fw->cfg.mac_mode != 0U);
        changed = true;
      }
    }
  }
  if (btn_edge) {
    if (hud_engine_menu_input(&fw->hud, HUD_MENU_ACT_OK, now_ms)) {
      keymap_set_mac_mode(&fw->keymap, fw->cfg.mac_mode != 0U);
      changed = true;
    }
  }
  if (was_open && !hud_engine_is_menu_open(&fw->hud) && fw->cfg.encoder_mode == 1U) {
    fw->cfg.encoder_mode = 0U;
    hud_engine_mark_options_dirty(&fw->hud);
  }
  if (changed) {
    sync_power_rails(fw);
    hud_engine_mark_options_dirty(&fw->hud);
    fw->hud.status_dirty = true;
    mark_eeprom_dirty(fw, now_ms);
  }
}

static void handle_encoder_step(tk75_fw_t *fw, encoder_events_t const *ev, uint32_t now_ms)
{
  tk75_persist_cfg_t *c = &fw->cfg;
  bool btn_edge = ev->btn_pressed && !fw->enc_prev_btn;
  fw->enc_prev_btn = ev->btn_pressed;

  if (ev->delta != 0 || btn_edge) {
    wake_from_sleep(fw, now_ms);
  }

  /* Push knob anytime during Hall calibration to cleanly abort */
  if (hall_cal_is_active(&fw->hall_cal)) {
    keymap_reset_encoder_state(&fw->keymap);
    if (btn_edge) {
      hall_cal_cancel(&fw->hall_cal);
    }
    return;
  }

  /* During TFT macro text editor: turn knob to move cursor, push knob to cancel/discard */
  if (macro_recorder_is_reviewing(&fw->macro_rec)) {
    keymap_reset_encoder_state(&fw->keymap);
    if (ev->delta != 0) {
      macro_recorder_editor_move(&fw->macro_rec, (int16_t)ev->delta);
    }
    if (btn_edge) {
      (void)macro_recorder_editor_cancel(&fw->macro_rec, &fw->hall);
    }
    return;
  }

  /* Overlay layer actions on (2,15) (e.g. Fn + Knob Push = KC_ENC_MODE_NEXT) must work from any mode. */
  if (btn_edge && keymap_try_encoder_overlay_action(&fw->keymap)) {
    keymap_reset_encoder_state(&fw->keymap);
    return;
  }

  if (c->encoder_mode == 1U) {
    keymap_reset_encoder_state(&fw->keymap);
    if (ev->delta == 0 && !btn_edge) {
      return;
    }
    route_encoder_to_hud(fw, ev, btn_edge, now_ms);
    return;
  }

  if (c->encoder_mode == 0U) {
    keymap_push_encoder(&fw->keymap, ev->delta, ev->btn_pressed);
    return;
  }

  keymap_reset_encoder_state(&fw->keymap);
  if (ev->delta == 0 && !btn_edge) {
    return;
  }

  bool changed = false;
  if (c->encoder_mode == 2U) {
    if (ev->delta != 0) {
      c->rgb_brightness = clamp_u8_step((int32_t)c->rgb_brightness + (ev->delta * 16), 0U, 255U);
      changed           = true;
    }
    if (btn_edge) {
      c->rgb_enabled ^= 1U;
      sync_power_rails(fw);
      changed         = true;
    }
  } else if (c->encoder_mode == 3U) {
    if (ev->delta != 0) {
      c->hall_actuation_um = clamp_u16_range((int32_t)c->hall_actuation_um + (ev->delta * 50), 100U, 3900U);
      apply_hall_config(fw);
      changed = true;
    }
    if (btn_edge) {
      c->hall_rt_enabled ^= 1U;
      apply_hall_config(fw);
      changed = true;
    }
  }

  if (changed) {
    fw->hud.status_dirty = true;
    mark_eeprom_dirty(fw, now_ms);
  }
}

static uint16_t compensated_vbat_adc(tk75_fw_t const *fw)
{
  bool caps_side_on = (fw->cfg.caps_side_enabled != 0U) && ((active_host_leds(fw) & 0x02U) != 0U);
  bool rgb_rail_on  = !fw->asleep && (fw->cfg.rgb_enabled != 0U || fw->cfg.side_enabled != 0U || caps_side_on);
  uint32_t ch_sum   = 0U;
  if (rgb_rail_on) {
    for (uint32_t i = 0U; i < RGB_LED_COUNT; i++) {
      ch_sum += (uint32_t)fw->leds[i].r + (uint32_t)fw->leds[i].g + (uint32_t)fw->leds[i].b;
    }
  }
  tk75_vbat_load_t load = {
    .tft_on             = fw->tft_rail_on,
    .tft_brightness_pct = fw->cfg.tft_brightness,
    .rgb_rail_on        = rgb_rail_on,
    .rgb_channel_sum    = ch_sum,
  };
  return tk75_battery_compensate_vbat(adc_scanner_get_vbat_raw(), &load);
}

static void poll_encoder_and_switches(tk75_fw_t *fw, uint32_t now_ms)
{
  encoder_events_t ev = encoder_hw_poll();
  handle_encoder_step(fw, &ev, now_ms);

  mode_switch_state_t sw = mode_switches_read();
  if (sw.conn_mode != fw->active_conn_mode) {
    wake_from_sleep(fw, now_ms);
    apply_transport_mode(fw, sw.conn_mode);
  }
  if (sw.vbus_present && !fw->vbus_present) {
    wake_from_sleep(fw, now_ms);
  }
  bool pwr_edge    = (sw.vbus_present != fw->vbus_present) || (sw.charging != fw->hw_charging);
  fw->vbus_present = sw.vbus_present;
  fw->hw_charging  = sw.charging;

  uint16_t vbat_raw = adc_scanner_get_vbat_raw();
  if ((!fw->batt_gauge.initialized && vbat_raw != 0U) ||
      pwr_edge ||
      (uint32_t)(now_ms - fw->last_batt_ms) >= 1000U) {
    fw->last_batt_ms = now_ms;
    uint16_t vbat_comp = compensated_vbat_adc(fw);
    fw->battery_pct  = tk75_battery_gauge_step(&fw->batt_gauge, vbat_comp, fw->vbus_present, fw->hw_charging);
    fw->charging     = tk75_battery_gauge_is_charging(&fw->batt_gauge);
  }

  bool mac_active = (fw->cfg.mac_mode != 0U);
  keymap_set_mac_mode(&fw->keymap, mac_active);
  spi3_rf_state_t const *rf = spi3_rf_get_state();
  hud_status_t st = {
    .active_layer = fw->sent_kbd.active_layer,
    .battery_pct  = fw->battery_pct,
    .host_leds    = active_host_leds(fw),
    .mac_mode     = mac_active,
    .conn_mode    = fw->active_conn_mode,
    .bt_slot      = fw->cfg.rf_bt_slot,
    .rf_state     = rf->link_state,
    .charging     = fw->charging,
    .vbus_present = fw->vbus_present,
  };
  hud_engine_set_status(&fw->hud, &st);
}

static void on_rf_via_rx_packet(uint8_t const *pkt64, uint8_t len, void *ctx)
{
  (void)len;
  (void)ctx;
  on_via_rx_packet(pkt64, VIA_RAW_EPSIZE);
}

static bool kbd_report_differs(keymap_report_t const *a, keymap_report_t const *b, bool boot_proto)
{
  if (a->modifiers != b->modifiers) {
    return true;
  }
  if (boot_proto) {
    return memcmp(a->boot_keys, b->boot_keys, sizeof(a->boot_keys)) != 0;
  }
  return memcmp(a->nkro_bitmap, b->nkro_bitmap, sizeof(a->nkro_bitmap)) != 0;
}

static void flush_consumer_usage(tk75_fw_t *fw,
                                 uint16_t requested_usage,
                                 bool (*send_fn)(uint16_t))
{
  uint16_t target = (requested_usage != 0U) ? requested_usage : fw->pending_consumer;
  if (target == fw->sent_consumer) {
    return;
  }
  if (send_fn(target)) {
    fw->sent_consumer = target;
    if (target == fw->pending_consumer || requested_usage != 0U) {
      fw->pending_consumer = 0U;
    }
  } else if (requested_usage != 0U) {
    fw->pending_consumer = requested_usage;
  }
}

static void flush_hid_reports(tk75_fw_t *fw, keymap_report_t const *rep, uint32_t now_ms)
{
  if (rep->pressed_count > fw->prev_pressed_count) {
    hud_engine_note_keystroke(&fw->hud, now_ms);
    fw->session_keys += (uint32_t)(rep->pressed_count - fw->prev_pressed_count);
  }
  fw->prev_pressed_count = rep->pressed_count;

  if (rep->pressed_count > 0U || rep->modifiers != 0U ||
      rep->consumer_usage != 0U || rep->system_usage != 0U) {
    wake_from_sleep(fw, now_ms);
  }

  if (fw->active_conn_mode != TK75_CONN_USB) {
    spi3_rf_state_t const *rf = spi3_rf_get_state();
    if (rf->sleeping || !rf->link_up) {
      return;
    }

    bool use_nkro = (fw->active_conn_mode == TK75_CONN_24G) && (rep->pressed_count > 6U);
    if (!use_nkro && fw->rf_nkro_active) {
      uint8_t zero15[15] = { 0 };
      if (spi3_rf_send_nkro_kbd(zero15)) {
        fw->rf_nkro_active = false;
        fw->has_sent_kbd   = false;
      }
    }

    if (!fw->has_sent_kbd || kbd_report_differs(rep, &fw->sent_kbd, !use_nkro)) {
      bool sent = false;
      if (use_nkro) {
        bool boot_ok = true;
        if (!fw->rf_nkro_active || !fw->has_sent_kbd ||
            rep->modifiers != fw->sent_kbd.modifiers) {
          uint8_t zero6[6] = { 0 };
          boot_ok = spi3_rf_send_boot_kbd(rep->modifiers, zero6);
        }
        sent = boot_ok && spi3_rf_send_nkro_kbd(rep->nkro_bitmap);
      } else {
        sent = spi3_rf_send_boot_kbd(rep->modifiers, rep->boot_keys);
      }
      if (sent) {
        fw->rf_nkro_active = use_nkro;
        fw->sent_kbd       = *rep;
        fw->has_sent_kbd   = true;
      }
    } else {
      fw->sent_kbd.active_layer = rep->active_layer;
    }

    flush_consumer_usage(fw, rep->consumer_usage, spi3_rf_send_consumer);
    if (rep->system_usage != fw->sent_system && spi3_rf_send_system(rep->system_usage)) {
      fw->sent_system = rep->system_usage;
    }
    if (fw->via_reply_pending) {
      fw->via_reply_pending = !send_via_reply(fw);
    }
    return;
  }

  bool boot_proto = usb_hid_is_boot_protocol();
  if (boot_proto != fw->prev_boot_proto) {
    uint8_t zero6[6]   = { 0 };
    uint8_t zero16[16] = { 0 };
    if (boot_proto) {
      usb_hid_send_nkro(0U, zero16);
    } else {
      usb_hid_send_boot_kbd(0U, zero6);
    }
    fw->prev_boot_proto = boot_proto;
    fw->has_sent_kbd    = false;
  }

  if (!fw->has_sent_kbd || kbd_report_differs(rep, &fw->sent_kbd, boot_proto)) {
    bool sent = boot_proto
      ? usb_hid_send_boot_kbd(rep->modifiers, rep->boot_keys)
      : usb_hid_send_nkro(rep->modifiers, rep->nkro_bitmap);
    if (sent) {
      fw->sent_kbd     = *rep;
      fw->has_sent_kbd = true;
    }
  } else {
    fw->sent_kbd.active_layer = rep->active_layer;
  }

  flush_consumer_usage(fw, rep->consumer_usage, usb_hid_send_consumer);
  if (rep->system_usage != fw->sent_system && usb_hid_send_system(rep->system_usage)) {
    fw->sent_system = rep->system_usage;
  }

  if (fw->via_reply_pending) {
    fw->via_reply_pending = !send_via_reply(fw);
  }
}

static void service_housekeeping(tk75_fw_t *fw, uint32_t now_ms)
{
  if (fw->boot_jump_pending && (uint32_t)(now_ms - fw->boot_jump_ms) >= BOOT_JUMP_DELAY_MS) {
    if (fw->eeprom_dirty) {
      flash_eeprom_save(flash_eeprom_at32_backend(), &fw->cfg, &fw->eeprom_slot);
    }
    board_enter_bootloader();
  }

  if ((uint32_t)(now_ms - fw->last_enc_ms) >= ENCODER_POLL_PERIOD_MS) {
    fw->last_enc_ms = now_ms;
    poll_encoder_and_switches(fw, now_ms);
  }

  uint8_t cur_host_leds = active_host_leds(fw);
  if (cur_host_leds != fw->prev_host_leds) {
    fw->prev_host_leds = cur_host_leds;
    sync_power_rails(fw);
  }

  /* Keep WPM buckets & synced RTC wall clock ticking even while TFT is off or asleep */
  hud_engine_update_wpm(&fw->hud, now_ms);

  if (!fw->asleep && (uint32_t)(now_ms - fw->last_rgb_ms) >= RGB_REFRESH_PERIOD_MS && ws2812_is_ready()) {
    fw->last_rgb_ms = now_ms;
    bool saved_flash = (fw->macro_rec.saved_until_ms != 0U) &&
                       ((int32_t)(fw->macro_rec.saved_until_ms - now_ms) > 0);
    uint8_t typed_bmp[MACRO_TYPED_BITMAP_BYTES];
    macro_player_consume_typed_bitmap(&fw->macro, typed_bmp);
    rgb_render_ctx_t rctx = {
      .fx                 = &fw->rgb_fx,
      .cfg                = &fw->cfg,
      .hall               = &fw->hall,
      .active_layer       = fw->sent_kbd.active_layer,
      .layer_mask         = fw->keymap.layer_state,
      .host_leds          = active_host_leds(fw),
      .macro_rec_active   = macro_recorder_is_capturing(&fw->macro_rec),
      .macro_rec_slot     = fw->macro_rec.rec_slot,
      .macro_saved_flash  = saved_flash,
      .macro_saved_slot   = fw->macro_rec.saved_slot,
      .macro_typed_bitmap = typed_bmp,
      .cal_done_mask      = hall_cal_is_active(&fw->hall_cal) ? fw->hall_cal.done_mask : NULL,
      .cal_active_slot    = fw->hall_cal.pressed_slot,
      .now_ms             = now_ms,
    };
    rgb_effects_render(&rctx, fw->leds);
    ws2812_show_async(fw->leds);
  }

  if (!fw->asleep && fw->cfg.tft_enabled != 0U && fw->tft_raw_paused) {
    uart7_rx_frame_t rx;
    while (uart7_tft_poll_rx(&rx)) {
      uint8_t n = (rx.payload_len < (uint8_t)sizeof(fw->tft_raw_rx_payload))
                    ? rx.payload_len
                    : (uint8_t)sizeof(fw->tft_raw_rx_payload);
      fw->tft_raw_rx_cmd = rx.cmd;
      fw->tft_raw_rx_len = n;
      memset(fw->tft_raw_rx_payload, 0, sizeof(fw->tft_raw_rx_payload));
      if (n > 0U) {
        memcpy(fw->tft_raw_rx_payload, rx.payload, n);
      }
      fw->tft_raw_rx_ready = 1U;
    }
  } else if (!fw->asleep && fw->cfg.tft_enabled != 0U) {
    /* Poll UART7 RX continuously for sub-millisecond ACK pacing on Pixel HUD */
    uart7_rx_frame_t rx;
    while (uart7_tft_poll_rx(&rx)) {
      pixel_hud_on_rx(&fw->px_hud, &rx);
      if (hud_engine_handle_rx(&fw->hud, &rx)) {
        mark_eeprom_dirty(fw, now_ms);
      }
    }

    if (pixel_hud_is_active(&fw->px_hud)) {
      if (fw->hud.wpm_current > fw->peak_wpm) {
        fw->peak_wpm = fw->hud.wpm_current;
      }
      uint16_t max_travel   = 0U;
      bool     any_actuated = false;
      for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
        if (fw->hall.active_mask[i] == 0U) {
          continue;
        }
        uint16_t um = fw->hall.states[i].travel_um;
        if (um > max_travel) {
          max_travel = um;
        }
        if (fw->hall.states[i].logical_pressed) {
          any_actuated = true;
        }
      }
      if (any_actuated) {
        fw->last_activity_ms = now_ms;
      }
      bool show_spd = (fw->macro_spd_toast_until_ms != 0U) &&
                      ((int32_t)(fw->macro_spd_toast_until_ms - now_ms) > 0);
      pixel_hud_live_t live = {
        .active_layer      = fw->sent_kbd.active_layer,
        .battery_pct       = fw->battery_pct,
        .host_leds         = active_host_leds(fw),
        .conn_mode         = fw->active_conn_mode,
        .bt_slot           = fw->cfg.rf_bt_slot,
        .charging          = fw->charging,
        .vbus_present      = fw->vbus_present,
        .mac_mode          = (fw->cfg.mac_mode != 0U),
        .wpm               = fw->hud.wpm_current,
        .max_travel_um     = max_travel,
        .any_actuated      = any_actuated,
        .macro_recording   = macro_recorder_is_capturing(&fw->macro_rec) && !macro_recorder_is_reviewing(&fw->macro_rec),
        .macro_reviewing   = macro_recorder_is_reviewing(&fw->macro_rec),
        .macro_rec_slot    = fw->macro_rec.rec_slot,
        .macro_rec_bytes   = fw->macro_rec.len,
        .macro_edit_toks   = fw->macro_rec.tokens,
        .macro_edit_count  = fw->macro_rec.tok_count,
        .macro_edit_cursor = fw->macro_rec.cursor,
        .macro_edit_rev    = fw->macro_rec.edit_rev,
        .macro_speed_toast = show_spd ? macro_player_normalize_speed(fw->cfg.macro_speed_idx) : 0U,
        .rtc_valid         = fw->hud.rtc_valid,
        .rtc_hour          = fw->hud.rtc.hour,
        .rtc_min           = fw->hud.rtc.min,
        .rtc_sec           = fw->hud.rtc.sec,
        .uptime_sec        = now_ms / 1000U,
        .idle_ms           = (uint32_t)(now_ms - fw->last_activity_ms),
        .peak_wpm          = fw->peak_wpm,
        .session_keys      = fw->session_keys,
        .cal_active        = hall_cal_is_active(&fw->hall_cal),
        .cal_done_count    = fw->hall_cal.done_count,
        .cal_total_keys    = fw->hall_cal.total_keys,
        .cal_strength_pct  = fw->hall_cal.strength_pct,
        .cal_key_done      = fw->hall_cal.active_done,
      };
      memcpy(live.cal_label, fw->hall_cal.active_label, sizeof(live.cal_label));
      pixel_hud_set_live(&fw->px_hud, &live);
      pixel_hud_tick(&fw->px_hud, now_ms);
      if (fw->px_hud.macro_play_req < TK75_MACRO_COUNT) {
        macro_player_trigger(&fw->macro, fw->cfg.macros, fw->px_hud.macro_play_req, now_ms);
        fw->px_hud.macro_play_req = 0xFFU;
      }
      if (fw->cfg.encoder_mode == 1U && !pixel_hud_is_menu_open(&fw->px_hud)) {
        fw->cfg.encoder_mode = 0U;
      }
    } else {
      pixel_hud_tick(&fw->px_hud, now_ms);
      /* Wait until power-on probe settles (900ms) & pixel_hud queue drains before stock HUD runs */
      bool allow_stock = fw->px_hud.stock_ui_mode ||
                         (!fw->px_hud.detected && (uint32_t)(now_ms - fw->tft_power_on_ms) >= 900U);
      if (allow_stock && fw->px_hud.q_count == 0U && !fw->px_hud.awaiting_ack &&
          (uint32_t)(now_ms - fw->last_hud_ms) >= HUD_POLL_PERIOD_MS) {
        fw->last_hud_ms = now_ms;
        hud_engine_tick(&fw->hud, now_ms);
        if (fw->cfg.encoder_mode == 1U && !hud_engine_is_menu_open(&fw->hud)) {
          fw->cfg.encoder_mode = 0U;
          hud_engine_mark_options_dirty(&fw->hud);
        }
      }
    }
  }

  if (fw->eeprom_dirty && (uint32_t)(now_ms - fw->eeprom_dirty_ms) >= EEPROM_SAVE_DEBOUNCE_MS) {
    flash_eeprom_save(flash_eeprom_at32_backend(), &fw->cfg, &fw->eeprom_slot);
    fw->eeprom_dirty = false;
  }

  if (!fw->asleep && !fw->vbus_present &&
      fw->active_conn_mode != TK75_CONN_USB &&
      fw->cfg.sleep_timeout_min > 0U) {
    uint32_t timeout_ms = (uint32_t)fw->cfg.sleep_timeout_min * 60000UL;
    if ((uint32_t)(now_ms - fw->last_activity_ms) >= timeout_ms) {
      enter_battery_sleep(fw);
    }
  }
}

int main(void)
{
  memset(&g_fw, 0, sizeof(g_fw));

  board_init();
  mode_switches_init();
  encoder_hw_init();

  factory_cal_data_t cal;
  factory_cal_load_flash(&cal);
  g_fw.eeprom_slot = flash_eeprom_load(flash_eeprom_at32_backend(), &g_fw.cfg);
  /* Always start in standard Volume/Mute encoder mode on boot rather than trapping in menu mode. */
  g_fw.cfg.encoder_mode = 0U;

  /* Apply any user-calibrated per-key spans stored in EEPROM on top of factory cal */
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (g_fw.cfg.hall_user_span_adc[i] >= 250U && g_fw.cfg.hall_user_span_adc[i] <= 2400U) {
      cal.span_adc[i] = g_fw.cfg.hall_user_span_adc[i];
    }
  }

  hall_engine_init(&g_fw.hall, cal.rest_adc, cal.span_adc);
  hall_cal_init(&g_fw.hall_cal);
  apply_hall_config(&g_fw);

  adc_scanner_init();
  hall_engine_seed_idle_baseline(&g_fw.hall, adc_scanner_capture_blocking());

  mode_switch_state_t init_sw = mode_switches_read();
  g_fw.active_conn_mode = init_sw.conn_mode;
  g_fw.vbus_present     = init_sw.vbus_present;
  g_fw.hw_charging      = init_sw.charging;
  tk75_battery_gauge_init(&g_fw.batt_gauge);
  g_fw.battery_pct      = tk75_battery_gauge_step(&g_fw.batt_gauge, adc_scanner_get_vbat_raw(),
                                                  g_fw.vbus_present, g_fw.hw_charging);
  g_fw.charging         = tk75_battery_gauge_is_charging(&g_fw.batt_gauge);
  g_fw.last_batt_ms     = board_millis();
  g_fw.last_activity_ms = g_fw.last_batt_ms;

  keymap_engine_init(&g_fw.keymap, &g_fw.cfg, &g_fw.hall);
  keymap_set_mac_mode(&g_fw.keymap, g_fw.cfg.mac_mode != 0U);
  keymap_set_action_cb(&g_fw.keymap, on_keymap_action, &g_fw);
  macro_player_init(&g_fw.macro);
  macro_player_set_speed(&g_fw.macro, g_fw.cfg.macro_speed_idx);
  macro_recorder_init(&g_fw.macro_rec);

  g_fw.via.cfg       = &g_fw.cfg;
  g_fw.via.hall      = &g_fw.hall;
  g_fw.via.diag      = &g_fw.diag;
  g_fw.via.on_event  = on_via_event;
  g_fw.via.event_ctx = &g_fw;

  ws2812_init();
  uart7_tft_hw_init();
  sync_power_rails(&g_fw);
  /* Let LED/TFT power rails stabilize before seeding Hall rest baseline (prevents 0.3mm idle offset) */
  board_delay_us(15000U);
  hall_engine_seed_idle_baseline(&g_fw.hall, adc_scanner_capture_blocking());
  spi3_rf_hw_init(rf_mode_for_conn(g_fw.active_conn_mode, g_fw.cfg.rf_bt_slot), g_fw.cfg.rf_poll_rate_idx);

  hud_init_cfg_t hud_cfg = {
    .cfg      = &g_fw.cfg,
    .send_fn  = hud_uart7_tx_adapter,
    .send_ctx = &g_fw,
  };
  hud_engine_init(&g_fw.hud, &hud_cfg);
  pixel_hud_init(&g_fw.px_hud, &g_fw.cfg, hud_uart7_tx_adapter, &g_fw);

  usb_hid_init(on_via_rx_packet, NULL);
  board_usb_otghs_init();
  tusb_rhport_init_t dev_init = {
    .role  = TUSB_ROLE_DEVICE,
    .speed = TUSB_SPEED_HIGH,
  };
  tusb_init(BOARD_TUD_RHPORT, &dev_init);
  if (g_fw.active_conn_mode != TK75_CONN_USB) {
    tud_disconnect();
    board_usb_set_connect(false);
  }
  board_start_scan_timer(adc_scanner_start_frame_isr);

  while (1) {
    tud_task();
    uint32_t now_ms = board_millis();
    spi3_rf_service(now_ms, g_fw.battery_pct, on_rf_via_rx_packet, &g_fw);

    uint16_t const *frame = adc_scanner_poll_frame();
    if (frame != NULL) {
      hall_engine_process_frame(&g_fw.hall, frame);
    }

    if (g_fw.px_hud.cal_start_req) {
      g_fw.px_hud.cal_start_req = false;
      g_fw.cfg.encoder_mode     = 0U;
      hall_cal_start(&g_fw.hall_cal, &g_fw.hall);
    }

    if (hall_cal_is_active(&g_fw.hall_cal)) {
      if (hall_cal_step(&g_fw.hall_cal, &g_fw.hall, &g_fw.cfg) == HALL_CAL_EVT_FINISHED) {
        mark_eeprom_dirty(&g_fw, now_ms);
        memset(g_fw.keymap.prev_pressed, 0, sizeof(g_fw.keymap.prev_pressed));
        memset(g_fw.keymap.active_codes, 0, sizeof(g_fw.keymap.active_codes));
        g_fw.keymap.layer_state &= ~(1UL << 1U);
      }
      keymap_report_t empty_rep;
      memset(&empty_rep, 0, sizeof(empty_rep));
      flush_hid_reports(&g_fw, &empty_rep, now_ms);
      service_housekeeping(&g_fw, now_ms);
      continue;
    }

    macro_rec_tick_ctx_t mctx = {
      .hall               = &g_fw.hall,
      .cfg                = &g_fw.cfg,
      .layer_state        = g_fw.keymap.layer_state,
      .now_ms             = now_ms,
      .tft_review_enabled = (!g_fw.asleep && g_fw.cfg.tft_enabled != 0U && pixel_hud_is_active(&g_fw.px_hud)),
    };
    bool            was_cap = macro_recorder_is_capturing(&g_fw.macro_rec);
    macro_rec_evt_t mevt    = macro_recorder_tick(&g_fw.macro_rec, &mctx);
    if (mevt == MACRO_REC_EVT_PLAY_SLOT) {
      macro_player_trigger(&g_fw.macro, g_fw.cfg.macros, g_fw.macro_rec.play_slot, now_ms);
    } else if (mevt == MACRO_REC_EVT_SAVED) {
      mark_eeprom_dirty(&g_fw, now_ms);
    }
    bool now_cap = macro_recorder_is_capturing(&g_fw.macro_rec);
    if (was_cap && !now_cap) {
      memset(g_fw.keymap.prev_pressed, 0, sizeof(g_fw.keymap.prev_pressed));
      memset(g_fw.keymap.active_codes, 0, sizeof(g_fw.keymap.active_codes));
      g_fw.keymap.layer_state &= ~(1UL << 1U);
    }

    if (now_cap) {
      keymap_report_t empty_rep;
      memset(&empty_rep, 0, sizeof(empty_rep));
      flush_hid_reports(&g_fw, &empty_rep, now_ms);
    } else if (frame != NULL || g_fw.macro.active || g_fw.keymap.enc_pulse_ticks > 0U ||
               g_fw.keymap.pulse_consumer != 0U || g_fw.pending_consumer != 0U || g_fw.sent_consumer != 0U) {
      keymap_report_t rep;
      keymap_build_report(&g_fw.keymap, &rep);
      macro_player_step(&g_fw.macro, g_fw.cfg.macros, now_ms, &rep);
      if (g_fw.cfg.win_lock != 0U) {
        rep.modifiers &= (uint8_t)~((1U << 3U) | (1U << 7U));
      }
      flush_hid_reports(&g_fw, &rep, now_ms);
    }

    service_housekeeping(&g_fw, now_ms);
    if (g_fw.asleep) {
      board_wfi();
    }
  }
}
