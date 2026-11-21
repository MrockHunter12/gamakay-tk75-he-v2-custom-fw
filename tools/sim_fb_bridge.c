/*
 * Host simulation bridge for rendering 100% firmware-accurate TFT (128x128) &
 * 90-LED RGB animations directly from real AT32F405 C firmware modules.
 */

#include "flash_eeprom.h"
#include "hall_cal.h"
#include "hall_engine.h"
#include "keymap.h"
#include "macro_player.h"
#include "macro_recorder.h"
#include "pixel_hud.h"
#include "rgb_effects.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SIM_TFT_W 128U
#define SIM_TFT_H 128U
#define FONT_GLYPHS 95U
#define FONT_BYTES (FONT_GLYPHS * 12U)

typedef struct {
  tk75_persist_cfg_t  cfg;
  hall_engine_t       hall;
  hall_cal_t          hall_cal;
  rgb_effects_state_t rgb_fx;
  pixel_hud_t         px_hud;
  rgb_grb_t           leds[RGB_LED_COUNT];

  uint16_t            rest_adc[HALL_SLOT_COUNT];
  uint16_t            span_adc[HALL_SLOT_COUNT];
  uint16_t            cur_adc[HALL_SLOT_COUNT];

  uint8_t             font6x12[FONT_BYTES];
  uint16_t            fb565[SIM_TFT_H][SIM_TFT_W];     /* Staging FB inside U24 SRAM */
  uint16_t            disp565[SIM_TFT_H][SIM_TFT_W];   /* Visible ST7735S panel GRAM */
  uint8_t             tft_pwm_pct;                     /* Last cmd=0x24 backlight % */
  bool                tft_manual;
  bool                has_pending_ack;
  uart7_rx_frame_t    pending_ack;
  uint8_t             batt_pct;
  bool                vbus_present;
  bool                charging;
  bool                macro_reviewing;
  macro_edit_tok_t    edit_toks[MACRO_EDITOR_MAX_TOKS];
  uint16_t            edit_count;
  uint16_t            edit_cursor;
  uint16_t            edit_rev;
} tk75_sim_t;

static tk75_sim_t g_sim;

static inline void u24_put_px(uint32_t x, uint32_t y, uint16_t col565)
{
  if (x < SIM_TFT_W && y < SIM_TFT_H) {
    g_sim.fb565[y][x] = col565;
  }
}

static void u24_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t col565)
{
  uint32_t x_end = x + w;
  uint32_t y_end = y + h;
  if (x_end > SIM_TFT_W) x_end = SIM_TFT_W;
  if (y_end > SIM_TFT_H) y_end = SIM_TFT_H;
  for (uint32_t cy = y; cy < y_end; cy++) {
    for (uint32_t cx = x; cx < x_end; cx++) {
      g_sim.fb565[cy][cx] = col565;
    }
  }
}

static void u24_draw_glyph(uint32_t x, uint32_t y, uint8_t ch, uint16_t fg, uint16_t bg,
                           uint32_t scale, bool transp)
{
  if (ch < 0x20U || ch > 0x7EU) {
    ch = 0x20U;
  }
  uint8_t const *glyph = &g_sim.font6x12[(uint32_t)(ch - 0x20U) * 12U];
  for (uint32_t col = 0U; col < 6U; col++) {
    uint16_t bits = ((uint16_t)glyph[col * 2U] << 8U) | (uint16_t)glyph[col * 2U + 1U];
    for (uint32_t row = 0U; row < 12U; row++) {
      bool on = (bits & (0x8000U >> row)) != 0U;
      if (!on && transp) {
        continue;
      }
      uint16_t c = on ? fg : bg;
      if (scale == 1U) {
        u24_put_px(x + col, y + row, c);
      } else {
        u24_fill_rect(x + col * scale, y + row * scale, scale, scale, c);
      }
    }
  }
}

static void u24_flush(void)
{
  memcpy(g_sim.disp565, g_sim.fb565, sizeof(g_sim.disp565));
}

/* Simulated U24 UART7 command receiver matching tools/tft_patch/u24_patch.c v1.2 */
static bool sim_send_uart7(uart7_tx_frame_t const *frame, void *ctx)
{
  (void)ctx;
  uint8_t cmd = frame->cmd;
  uint16_t plen = frame->payload_len;
  uint8_t const *p = frame->payload;
  uart7_rx_frame_t ack;
  memset(&ack, 0, sizeof(ack));

  if (cmd == PIXEL_HUD_CMD_VERSION) {
    ack.cmd               = 0xA5U;
    ack.payload_len       = 3U;
    ack.payload[0]        = 0x00U;
    ack.payload[1]        = 1U;
    ack.payload[2]        = 2U;
    g_sim.pending_ack     = ack;
    g_sim.has_pending_ack = true;
    return true;
  }

  if (cmd == PIXEL_HUD_CMD_MANUAL && plen >= 1U) {
    g_sim.tft_manual = (p[0] != 0U);
    if (g_sim.tft_manual) {
      u24_fill_rect(0U, 0U, SIM_TFT_W, SIM_TFT_H, 0x0862U);
      u24_flush();
    }
  } else if (cmd == PIXEL_HUD_CMD_FILL_RECT && plen >= 7U) {
    uint16_t col = ((uint16_t)p[4] << 8U) | p[5];
    u24_fill_rect(p[0], p[1], p[2], p[3], col);
    if (p[6] != 0U) {
      u24_flush();
    }
  } else if (cmd == PIXEL_HUD_CMD_TEXT && plen >= 8U) {
    uint32_t x     = p[0];
    uint32_t y     = p[1];
    uint16_t fg    = ((uint16_t)p[2] << 8U) | p[3];
    uint16_t bg    = ((uint16_t)p[4] << 8U) | p[5];
    uint8_t  flags = p[6];
    uint32_t scale = flags & 3U;
    if (scale == 0U) scale = 1U;
    if (scale > 3U) scale = 3U;
    bool transp = (flags & 0x40U) != 0U;
    uint32_t step = 6U * scale;
    for (uint32_t i = 7U; i < plen; i++) {
      uint8_t ch = p[i];
      if (ch == 0U) break;
      u24_draw_glyph(x, y, ch, fg, bg, scale, transp);
      x += step;
      if (x >= SIM_TFT_W) break;
    }
    if ((flags & 0x80U) != 0U) {
      u24_flush();
    }
  } else if (cmd == PIXEL_HUD_CMD_BRIGHTNESS && plen >= 1U) {
    if (plen >= 2U && p[1] == 1U && p[0] <= 100U) {
      g_sim.tft_pwm_pct = p[0];
    } else if (p[0] == 0xFFU) {
      u24_flush();
    }
  } else if (cmd == PIXEL_HUD_CMD_BATCH_RECT && plen >= 2U) {
    uint8_t flags = p[0];
    uint8_t count = p[1];
    uint32_t off  = 2U;
    for (uint8_t i = 0U; i < count && (off + 6U) <= plen; i++, off += 6U) {
      uint16_t col = ((uint16_t)p[off + 4U] << 8U) | p[off + 5U];
      u24_fill_rect(p[off], p[off + 1U], p[off + 2U], p[off + 3U], col);
    }
    if ((flags & 1U) != 0U) {
      u24_flush();
    }
  }

  ack.cmd               = (uint8_t)(cmd | 0x80U);
  ack.payload_len       = 1U;
  ack.payload[0]        = 0x00U;
  g_sim.pending_ack     = ack;
  g_sim.has_pending_ack = true;
  return true;
}

static void sim_drain_hud(uint32_t now_ms)
{
  for (uint32_t i = 0U; i < 64U; i++) {
    pixel_hud_tick(&g_sim.px_hud, now_ms);
    if (g_sim.has_pending_ack) {
      g_sim.has_pending_ack = false;
      pixel_hud_on_rx(&g_sim.px_hud, &g_sim.pending_ack);
    } else if (g_sim.px_hud.q_count == 0U && !g_sim.px_hud.full_redraw_needed &&
               !g_sim.px_hud.body_redraw_needed && !g_sim.px_hud.tft_brt_dirty) {
      break;
    }
  }
}

int sim_init(char const *u24_bin_path)
{
  memset(&g_sim, 0, sizeof(g_sim));
  if (u24_bin_path != NULL) {
    FILE *f = fopen(u24_bin_path, "rb");
    if (f != NULL) {
      if (fseek(f, 0x16B3AL, SEEK_SET) == 0) {
        size_t n = fread(g_sim.font6x12, 1U, FONT_BYTES, f);
        (void)n;
      }
      fclose(f);
    }
  }
  return 0;
}

void sim_reset(void)
{
  uint8_t font_copy[FONT_BYTES];
  memcpy(font_copy, g_sim.font6x12, FONT_BYTES);
  memset(&g_sim, 0, sizeof(g_sim));
  memcpy(g_sim.font6x12, font_copy, FONT_BYTES);

  tk75_cfg_set_defaults(&g_sim.cfg);
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    g_sim.rest_adc[i] = 2400U;
    g_sim.span_adc[i] = 1000U;
    g_sim.cur_adc[i]  = 2400U;
  }
  hall_engine_init(&g_sim.hall, g_sim.rest_adc, g_sim.span_adc);
  hall_engine_seed_idle_baseline(&g_sim.hall, g_sim.cur_adc);
  hall_cal_init(&g_sim.hall_cal);

  pixel_hud_init(&g_sim.px_hud, &g_sim.cfg, sim_send_uart7, NULL);
  g_sim.tft_pwm_pct  = g_sim.cfg.tft_brightness;
  g_sim.vbus_present = true;
  g_sim.charging     = false;
  g_sim.batt_pct     = 100U;
  u24_fill_rect(0U, 0U, SIM_TFT_W, SIM_TFT_H, 0x0862U);
  u24_flush();

  /* Immediately detect patched U24 & drain initial boot draw */
  sim_drain_hud(100U);
  sim_drain_hud(120U);
}

void sim_set_rgb(uint8_t en, uint8_t brt, uint8_t fx, uint8_t spd, uint8_t hue, uint8_t sat)
{
  g_sim.cfg.rgb_enabled    = en;
  g_sim.cfg.rgb_brightness = brt;
  g_sim.cfg.rgb_effect     = fx;
  g_sim.cfg.rgb_speed      = spd;
  g_sim.cfg.rgb_hue        = hue;
  g_sim.cfg.rgb_sat        = sat;
  pixel_hud_mark_config_dirty(&g_sim.px_hud);
}

void sim_set_side(uint8_t en, uint8_t brt, uint8_t fx, uint8_t spd, uint8_t hue, uint8_t sat)
{
  g_sim.cfg.side_enabled    = en;
  g_sim.cfg.side_brightness = brt;
  g_sim.cfg.side_effect     = fx;
  g_sim.cfg.side_speed      = spd;
  g_sim.cfg.side_hue        = hue;
  g_sim.cfg.side_sat        = sat;
  pixel_hud_mark_config_dirty(&g_sim.px_hud);
}

void sim_set_caps_side(uint8_t en, uint8_t fx)
{
  g_sim.cfg.caps_side_enabled = en;
  g_sim.cfg.caps_side_effect  = fx;
  pixel_hud_mark_config_dirty(&g_sim.px_hud);
}

void sim_set_tft_cfg(uint8_t brt, uint8_t saver, uint8_t mac_mode)
{
  g_sim.cfg.tft_brightness  = brt;
  g_sim.cfg.tft_screensaver = saver;
  g_sim.cfg.mac_mode        = mac_mode;
  pixel_hud_mark_config_dirty(&g_sim.px_hud);
}

void sim_set_hall_cfg(uint16_t act_um, uint8_t rt_en, uint8_t socd)
{
  g_sim.cfg.hall_actuation_um = act_um;
  g_sim.cfg.hall_rt_enabled   = rt_en;
  g_sim.cfg.hall_socd_mode    = socd;
  hall_key_config_t hcfg = {
    .actuation_um       = act_um,
    .release_hyst_um    = g_sim.cfg.hall_hyst_um,
    .rt_enabled         = (rt_en != 0U),
    .rt_continuous      = false,
    .rt_down_um         = g_sim.cfg.hall_rt_down_um,
    .rt_up_um           = g_sim.cfg.hall_rt_up_um,
  };
  hall_engine_set_global_config(&g_sim.hall, &hcfg);
  pixel_hud_mark_config_dirty(&g_sim.px_hud);
}

void sim_set_power_state(uint8_t vbus, uint8_t charging, uint8_t batt_pct)
{
  g_sim.vbus_present = (vbus != 0U);
  g_sim.charging     = (charging != 0U);
  g_sim.batt_pct     = batt_pct;
}

void sim_set_macro_edit(uint8_t reviewing, uint8_t const *hid_mods_pairs, uint16_t count, uint16_t cursor)
{
  g_sim.macro_reviewing = (reviewing != 0U);
  if (count > MACRO_EDITOR_MAX_TOKS) {
    count = MACRO_EDITOR_MAX_TOKS;
  }
  g_sim.edit_count  = count;
  g_sim.edit_cursor = (cursor <= count) ? cursor : count;
  if (hid_mods_pairs != NULL) {
    for (uint16_t i = 0U; i < count; i++) {
      g_sim.edit_toks[i].hid  = hid_mods_pairs[i * 2U + 0U];
      g_sim.edit_toks[i].mods = hid_mods_pairs[i * 2U + 1U];
    }
  }
  g_sim.edit_rev++;
}

void sim_set_macro_slot_used(uint8_t slot)
{
  uint8_t const dummy[3] = { 0x01U, 0x01U, (uint8_t)KC_H };
  macro_recorder_save_slot(g_sim.cfg.macros, slot, dummy, 3U);
}

void sim_clear_all_keys(void)
{
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    g_sim.cur_adc[i] = g_sim.rest_adc[i];
  }
}

void sim_set_key_travel(uint8_t row, uint8_t col, uint16_t travel_um)
{
  if (row >= HALL_ROWS || col >= HALL_COLS) {
    return;
  }
  uint8_t slot = (uint8_t)(row * HALL_COLS + col);
  if (travel_um > 4000U) {
    travel_um = 4000U;
  }
  uint32_t drop = ((uint32_t)travel_um * (uint32_t)g_sim.span_adc[slot]) / 4000U;
  g_sim.cur_adc[slot] = (uint16_t)(g_sim.rest_adc[slot] - drop);
}

void sim_hud_open_menu(uint32_t now_ms)
{
  pixel_hud_open_menu(&g_sim.px_hud, now_ms);
}

void sim_hud_close_menu(void)
{
  pixel_hud_close_menu(&g_sim.px_hud);
}

void sim_hud_input(uint8_t act, uint32_t now_ms)
{
  pixel_hud_input(&g_sim.px_hud, (hud_menu_act_t)act, now_ms);
}

void sim_cal_start(void)
{
  hall_cal_start(&g_sim.hall_cal, &g_sim.hall);
}

void sim_cal_mark_done(uint8_t row, uint8_t col)
{
  if (row < HALL_ROWS && col < HALL_COLS) {
    uint8_t slot = (uint8_t)(row * HALL_COLS + col);
    if (g_sim.hall.active_mask[slot] && g_sim.hall_cal.done_mask[slot] == 0U) {
      g_sim.hall_cal.done_mask[slot] = 1U;
      g_sim.hall_cal.done_count++;
    }
  }
}

void sim_step(uint32_t now_ms,
              uint16_t wpm,
              uint16_t peak_wpm,
              uint32_t session_keys,
              uint8_t  host_leds,
              uint8_t  active_layer,
              uint32_t layer_mask,
              uint8_t  macro_rec,
              uint8_t  macro_slot,
              uint16_t macro_bytes,
              uint8_t  macro_saved_flash,
              uint8_t  macro_toast,
              uint32_t idle_ms,
              uint8_t  rtc_h,
              uint8_t  rtc_m,
              uint8_t  rtc_s,
              uint8_t const *typed_bmp32)
{
  /* Settle Hall EMA quickly toward requested ADC positions */
  for (uint8_t k = 0U; k < 12U; k++) {
    hall_engine_process_frame(&g_sim.hall, g_sim.cur_adc);
  }
  if (hall_cal_is_active(&g_sim.hall_cal)) {
    hall_cal_step(&g_sim.hall_cal, &g_sim.hall, &g_sim.cfg);
  }

  uint16_t max_travel   = 0U;
  bool     any_actuated = false;
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (g_sim.hall.active_mask[i] == 0U) continue;
    if (g_sim.hall.states[i].travel_um > max_travel) {
      max_travel = g_sim.hall.states[i].travel_um;
    }
    if (g_sim.hall.states[i].logical_pressed) {
      any_actuated = true;
    }
  }

  pixel_hud_live_t live;
  memset(&live, 0, sizeof(live));
  live.active_layer      = active_layer;
  live.battery_pct       = (g_sim.batt_pct != 0U) ? g_sim.batt_pct : 100U;
  live.host_leds         = host_leds;
  live.conn_mode         = g_sim.vbus_present ? 0U : 1U; /* USB when plugged, 2.4G on battery */
  live.vbus_present      = (g_sim.batt_pct != 0U) ? g_sim.vbus_present : true;
  live.charging          = g_sim.charging;
  live.mac_mode          = (g_sim.cfg.mac_mode != 0U);
  live.wpm               = wpm;
  live.max_travel_um     = max_travel;
  live.any_actuated      = any_actuated;
  live.macro_recording   = (macro_rec != 0U) && !g_sim.macro_reviewing;
  live.macro_reviewing   = g_sim.macro_reviewing;
  live.macro_rec_slot    = macro_slot;
  live.macro_rec_bytes   = macro_bytes;
  live.macro_edit_toks   = g_sim.edit_toks;
  live.macro_edit_count  = g_sim.edit_count;
  live.macro_edit_cursor = g_sim.edit_cursor;
  live.macro_edit_rev    = g_sim.edit_rev;
  live.macro_speed_toast = macro_toast;
  live.rtc_valid         = true;
  live.rtc_hour          = rtc_h;
  live.rtc_min           = rtc_m;
  live.rtc_sec           = rtc_s;
  live.uptime_sec        = now_ms / 1000U;
  live.idle_ms           = idle_ms;
  live.peak_wpm          = peak_wpm;
  live.session_keys      = session_keys;
  live.cal_active        = hall_cal_is_active(&g_sim.hall_cal);
  live.cal_done_count    = g_sim.hall_cal.done_count;
  live.cal_total_keys    = g_sim.hall_cal.total_keys;
  live.cal_strength_pct  = g_sim.hall_cal.strength_pct;
  live.cal_key_done      = g_sim.hall_cal.active_done;
  memcpy(live.cal_label, g_sim.hall_cal.active_label, sizeof(live.cal_label));

  pixel_hud_set_live(&g_sim.px_hud, &live);
  /* Drain full draw queue smoothly at current timestamp */
  sim_drain_hud(now_ms);

  rgb_render_ctx_t rctx = {
    .fx                 = &g_sim.rgb_fx,
    .cfg                = &g_sim.cfg,
    .hall               = &g_sim.hall,
    .active_layer       = active_layer,
    .layer_mask         = layer_mask,
    .host_leds          = host_leds,
    .macro_rec_active   = (macro_rec != 0U),
    .macro_rec_slot     = macro_slot,
    .macro_saved_flash  = (macro_saved_flash != 0U),
    .macro_saved_slot   = macro_slot,
    .macro_typed_bitmap = typed_bmp32,
    .cal_done_mask      = hall_cal_is_active(&g_sim.hall_cal) ? g_sim.hall_cal.done_mask : NULL,
    .cal_active_slot    = g_sim.hall_cal.pressed_slot,
    .now_ms             = now_ms,
  };
  rgb_effects_render(&rctx, g_sim.leds);
}

void sim_get_tft_rgb(uint8_t *out_rgb)
{
  uint32_t idx = 0U;
  for (uint32_t y = 0U; y < SIM_TFT_H; y++) {
    for (uint32_t x = 0U; x < SIM_TFT_W; x++) {
      uint16_t c = g_sim.disp565[y][x];
      uint8_t r5 = (uint8_t)((c >> 11U) & 0x1FU);
      uint8_t g6 = (uint8_t)((c >> 5U) & 0x3FU);
      uint8_t b5 = (uint8_t)(c & 0x1FU);
      out_rgb[idx++] = (uint8_t)((r5 << 3U) | (r5 >> 2U));
      out_rgb[idx++] = (uint8_t)((g6 << 2U) | (g6 >> 4U));
      out_rgb[idx++] = (uint8_t)((b5 << 3U) | (b5 >> 2U));
    }
  }
}

uint8_t sim_get_tft_pwm(void)
{
  return g_sim.tft_pwm_pct;
}

void sim_get_leds_rgb(uint8_t *out_rgb90)
{
  for (uint32_t i = 0U; i < RGB_LED_COUNT; i++) {
    out_rgb90[i * 3U + 0U] = g_sim.leds[i].r;
    out_rgb90[i * 3U + 1U] = g_sim.leds[i].g;
    out_rgb90[i * 3U + 2U] = g_sim.leds[i].b;
  }
}

uint8_t sim_led_idx(uint8_t row, uint8_t col)
{
  return rgb_matrix_to_led_idx(row, col);
}
