#include "hud_engine.h"
#include "mode_switches.h"
#include "spi3_rf.h"
#include <string.h>

#define HUD_STATUS_INTERVAL_MS    100U
#define HUD_HEARTBEAT_INTERVAL_MS 1000U
#define HUD_BUCKET_INTERVAL_MS    1000U
#define HUD_MAX_RGB_FX            7U
#define HUD_MAX_SIDE_FX           4U
#define HUD_MAX_TFT_COLOR         9U
#define HUD_MAX_TFT_PAGE          6U
#define HUD_TFT_CONN_24G          3U
#define HUD_TFT_CONN_USB          4U
#define HUD_TFT_BT_SLOTS          3U
#define HUD_TFT_RF_NONE           0U
#define HUD_TFT_RF_CONNECTED      1U
#define HUD_TFT_RF_CONNECTING     2U
#define HUD_TFT_RF_DISCONNECTED   3U
#define HUD_TFT_CHG_BATTERY       0U
#define HUD_TFT_CHG_USB_FULL      1U
#define HUD_TFT_CHG_ACTIVE        2U
#define HUD_RGB_BRIGHT_MAX_STEP   4U
#define HUD_RGB_SPEED_MAX_STEP    4U
#define HUD_SIDE_BRIGHT_MAX_STEP  4U
#define HUD_SIDE_SPEED_MAX_STEP   3U

static uint8_t days_in_month(uint16_t year, uint8_t month)
{
  static uint8_t const k_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
  if (month < 1U || month > 12U) {
    return 30U;
  }
  if (month == 2U && ((year % 4U) == 0U && ((year % 100U) != 0U || (year % 400U) == 0U))) {
    return 29U;
  }
  return k_days[month - 1U];
}

static void advance_rtc_one_second(via_rtc_time_t *rtc)
{
  if (++rtc->sec < 60U) {
    return;
  }
  rtc->sec = 0U;
  if (++rtc->min < 60U) {
    return;
  }
  rtc->min = 0U;
  if (++rtc->hour < 24U) {
    return;
  }
  rtc->hour = 0U;
  if (++rtc->day <= days_in_month(rtc->year, rtc->month)) {
    return;
  }
  rtc->day = 1U;
  if (++rtc->month <= 12U) {
    return;
  }
  rtc->month = 1U;
  rtc->year++;
}

static void update_wpm_buckets(hud_engine_t *hud, uint32_t now_ms)
{
  while ((uint32_t)(now_ms - hud->last_bucket_ms) >= HUD_BUCKET_INTERVAL_MS) {
    hud->last_bucket_ms += HUD_BUCKET_INTERVAL_MS;
    hud->wpm_bucket_idx = (uint8_t)((hud->wpm_bucket_idx + 1U) % HUD_WPM_BUCKETS);
    hud->wpm_buckets[hud->wpm_bucket_idx] = 0U;
  }

  uint32_t total_chars = 0U;
  for (uint8_t i = 0U; i < HUD_WPM_BUCKETS; i++) {
    total_chars += hud->wpm_buckets[i];
  }
  /* 10 s sliding window * 6 = chars/min; / 5 = standard WPM */
  uint16_t wpm = (uint16_t)((total_chars * 6U) / 5U);
  if (wpm != hud->wpm_current) {
    hud->wpm_current = wpm;
  }
}

static bool send_hud_pkt(hud_engine_t const *hud, uint8_t cmd, uint8_t const *payload, uint16_t len)
{
  uart7_tx_frame_t frame = {
    .cmd         = cmd,
    .payload_len = len,
    .payload     = payload,
  };
  return hud->send_fn(&frame, hud->send_ctx);
}

static uint8_t rgb_brightness_step(uint8_t brightness)
{
  if (brightness == 0U) {
    return 0U;
  }
  uint8_t step = (uint8_t)(((uint16_t)brightness + 63U) >> 6U);
  return (step > HUD_RGB_BRIGHT_MAX_STEP) ? HUD_RGB_BRIGHT_MAX_STEP : step;
}

static uint8_t quantize_u8_step(uint8_t val, uint8_t max_step)
{
  if (val == 0U || max_step == 0U) {
    return 0U;
  }
  if (max_step == 4U) {
    return rgb_brightness_step(val);
  }
  uint8_t step = (uint8_t)(((uint16_t)val + 84U) / 85U);
  return (step > max_step) ? max_step : step;
}

static uint8_t step_to_u8(uint8_t step, uint8_t max_step)
{
  static uint8_t const k_steps4[5] = { 0U, 64U, 128U, 192U, 255U };
  static uint8_t const k_steps3[4] = { 0U, 85U, 170U, 255U };
  if (max_step == 3U) {
    return k_steps3[(step <= 3U) ? step : 3U];
  }
  return k_steps4[(step <= 4U) ? step : 4U];
}

typedef struct {
  uint8_t hue;
  uint8_t sat;
} tft_color_hs_t;

static tft_color_hs_t const k_tft_colors[HUD_MAX_TFT_COLOR] = {
  {   0U, 255U }, /* 0: Red */
  {  24U, 255U }, /* 1: Orange */
  {  43U, 255U }, /* 2: Yellow */
  {  85U, 255U }, /* 3: Green */
  { 128U, 255U }, /* 4: Cyan */
  { 170U, 255U }, /* 5: Blue */
  { 213U, 255U }, /* 6: Purple */
  { 235U, 255U }, /* 7: Pink */
  {   0U,   0U }, /* 8: White */
};

static uint8_t color_idx_from_hs(uint8_t hue, uint8_t sat)
{
  if (sat < 64U) {
    return 8U;
  }
  uint8_t best_idx  = 0U;
  uint8_t best_dist = 255U;
  for (uint8_t i = 0U; i < 8U; i++) {
    uint8_t d    = (uint8_t)(hue - k_tft_colors[i].hue);
    uint8_t dist = (d > 128U) ? (uint8_t)(256U - d) : d;
    if (dist < best_dist) {
      best_dist = dist;
      best_idx  = i;
    }
  }
  return best_idx;
}

static uint8_t adjust_wrap_u8(uint8_t cur, uint8_t count, hud_menu_act_t act)
{
  if (count == 0U) {
    return 0U;
  }
  if (act == HUD_MENU_ACT_NEXT) {
    return (uint8_t)((cur + 1U) % count);
  }
  if (act == HUD_MENU_ACT_PREV) {
    return (uint8_t)((cur + count - 1U) % count);
  }
  return (cur < count) ? cur : 0U;
}

static uint8_t adjust_clamp_u8(uint8_t cur, uint8_t max_val, hud_menu_act_t act)
{
  if (act == HUD_MENU_ACT_NEXT) {
    return (cur < max_val) ? (uint8_t)(cur + 1U) : max_val;
  }
  if (act == HUD_MENU_ACT_PREV) {
    return (cur > 0U) ? (uint8_t)(cur - 1U) : 0U;
  }
  return (cur <= max_val) ? cur : max_val;
}

static uint8_t map_tft_conn_icon(hud_status_t const *st)
{
  if (st->conn_mode == TK75_CONN_USB) {
    return HUD_TFT_CONN_USB;
  }
  if (st->conn_mode == TK75_CONN_24G) {
    return HUD_TFT_CONN_24G;
  }
  return (st->bt_slot < HUD_TFT_BT_SLOTS) ? st->bt_slot : 0U;
}

static uint8_t map_tft_rf_status(hud_status_t const *st)
{
  if (st->conn_mode == TK75_CONN_USB) {
    return HUD_TFT_RF_NONE;
  }
  if (st->rf_state == SPI3_RF_STATE_CONNECTED) {
    return HUD_TFT_RF_CONNECTED;
  }
  if (st->rf_state == SPI3_RF_STATE_PAIRING || st->rf_state == SPI3_RF_STATE_RECONNECTING) {
    return HUD_TFT_RF_CONNECTING;
  }
  return HUD_TFT_RF_DISCONNECTED;
}

static uint8_t map_tft_charge_state(hud_status_t const *st)
{
  if (st->charging) {
    return HUD_TFT_CHG_ACTIVE;
  }
  if (st->vbus_present || st->conn_mode == TK75_CONN_USB) {
    return HUD_TFT_CHG_USB_FULL;
  }
  return HUD_TFT_CHG_BATTERY;
}

static bool send_status_packet(hud_engine_t *hud)
{
  tk75_persist_cfg_t const *c = hud->cfg;
  /* Stock app.c UART7 cmd=0x01 (18B payload): p[0..1]=menu cursor LE, p[2]=trigger,
   * p[3]=pic slot, p[4..5]=TFT brightness down/up toggle bits, p[6..7]=lang/mac,
   * p[8..11]=rgb, p[12..15]=side. */
  uint8_t p[18];
  memset(p, 0, sizeof(p));

  p[0]  = (uint8_t)(hud->menu_cursor & 0xFFU);
  p[1]  = (uint8_t)((hud->menu_cursor >> 8U) & 0xFFU);
  p[2]  = hud->menu_trigger;
  p[3]  = hud->menu_pic_slot;
  p[4]  = hud->menu_tft_brt_dn & 1U;
  p[5]  = hud->menu_tft_brt_up & 1U;
  p[6]  = hud->menu_lang_dirty;
  p[7]  = (hud->status.mac_mode || c->mac_mode != 0U) ? 1U : 0U;
  p[8]  = c->rgb_effect;
  p[9]  = color_idx_from_hs(c->rgb_hue, c->rgb_sat);
  p[10] = rgb_brightness_step(c->rgb_brightness);
  p[11] = quantize_u8_step(c->rgb_speed, HUD_RGB_SPEED_MAX_STEP);
  p[12] = c->side_effect;
  p[13] = color_idx_from_hs(c->side_hue, c->side_sat);
  p[14] = quantize_u8_step(c->side_brightness, HUD_SIDE_BRIGHT_MAX_STEP);
  p[15] = quantize_u8_step(c->side_speed, HUD_SIDE_SPEED_MAX_STEP);

  bool ok = send_hud_pkt(hud, HUD_CMD_STATUS, p, (uint16_t)sizeof(p));
  if (ok) {
    hud->sent_tft_brt_dn = p[4];
    hud->sent_tft_brt_up = p[5];
  }
  return ok;
}

static bool send_options_packet(hud_engine_t *hud)
{
  tk75_persist_cfg_t const *c = hud->cfg;
  /* Stock app.c cmd=0x02 (12B payload) updates home-screen status icons. */
  uint8_t p[12];
  memset(p, 0, sizeof(p));

  p[0]  = map_tft_conn_icon(&hud->status);
  p[1]  = map_tft_rf_status(&hud->status);
  p[2]  = map_tft_charge_state(&hud->status);
  p[3]  = hud->status.battery_pct;
  p[4]  = (hud->status.mac_mode || c->mac_mode != 0U) ? 1U : 0U;
  p[6]  = (uint8_t)(hud->status.host_leds & 0x07U);
  p[10] = hud->status.mac_mode ? 1U : 0U;
  p[11] = (c->win_lock != 0U) ? 1U : 0U;

  return send_hud_pkt(hud, HUD_CMD_OPTIONS, p, (uint16_t)sizeof(p));
}

static bool send_rtc_packet(hud_engine_t *hud)
{
  uint8_t p[7];
  p[0] = (uint8_t)(hud->rtc.year & 0xFFU);
  p[1] = (uint8_t)(hud->rtc.year >> 8U);
  p[2] = hud->rtc.month;
  p[3] = hud->rtc.day;
  p[4] = hud->rtc.hour;
  p[5] = hud->rtc.min;
  p[6] = hud->rtc.sec;
  return send_hud_pkt(hud, HUD_CMD_RTC, p, (uint16_t)sizeof(p));
}

void hud_engine_init(hud_engine_t *hud, hud_init_cfg_t const *init)
{
  if (hud == NULL || init == NULL) {
    return;
  }
  memset(hud, 0, sizeof(*hud));
  hud->cfg                = init->cfg;
  hud->send_fn            = init->send_fn;
  hud->send_ctx           = init->send_ctx;
  hud->status.battery_pct  = 100U;
  hud->status.conn_mode    = TK75_CONN_USB;
  hud->status.vbus_present = true;
  hud->status_dirty        = true;
  hud->options_dirty       = true;
}

void hud_engine_note_keystroke(hud_engine_t *hud, uint32_t now_ms)
{
  if (hud == NULL) {
    return;
  }
  update_wpm_buckets(hud, now_ms);
  if (hud->wpm_buckets[hud->wpm_bucket_idx] < 65000U) {
    hud->wpm_buckets[hud->wpm_bucket_idx]++;
  }
}

void hud_engine_set_status(hud_engine_t *hud, hud_status_t const *st)
{
  if (hud == NULL || st == NULL) {
    return;
  }
  if (hud->status.active_layer != st->active_layer ||
      hud->status.battery_pct  != st->battery_pct ||
      hud->status.host_leds    != st->host_leds ||
      hud->status.mac_mode     != st->mac_mode ||
      hud->status.conn_mode    != st->conn_mode ||
      hud->status.bt_slot      != st->bt_slot ||
      hud->status.rf_state     != st->rf_state ||
      hud->status.charging     != st->charging ||
      hud->status.vbus_present != st->vbus_present) {
    hud->status        = *st;
    hud->options_dirty = true;
  }
}

void hud_engine_mark_options_dirty(hud_engine_t *hud)
{
  if (hud != NULL) {
    hud->options_dirty = true;
    hud->status_dirty  = true;
  }
}

void hud_engine_set_rtc(hud_engine_t *hud, via_rtc_time_t const *rtc)
{
  if (hud == NULL || rtc == NULL) {
    return;
  }
  hud->rtc             = *rtc;
  hud->rtc_valid       = true;
  hud->rtc_tick_primed = false;
  hud->rtc_dirty       = true;
}

static bool send_media_chunk_frame(hud_engine_t *hud, uint8_t slice_len)
{
  /* Stock TFT UART7 cmd 0x04 payload: 8-byte frame/chunk header + up to 56B RGB565. */
  hud->media_buf[0] = hud->media_slot;
  hud->media_buf[1] = hud->media_frame_idx;
  hud->media_buf[2] = hud->media_total_frames;
  hud->media_buf[3] = hud->media_delay_units;
  hud->media_buf[4] = (uint8_t)(hud->media_chunk_idx & 0xFFU);
  hud->media_buf[5] = (uint8_t)(hud->media_chunk_idx >> 8U);
  hud->media_buf[6] = slice_len;
  hud->media_buf[7] = 0U;
  bool ok = send_hud_pkt(hud, HUD_CMD_MEDIA, hud->media_buf, 64U);
  if (ok) {
    hud->media_chunk_idx++;
    hud->media_fill = 0U;
    memset(&hud->media_buf[8], 0, 56U);
  }
  return ok;
}

bool hud_engine_push_media_chunk(hud_engine_t *hud, via_media_chunk_t const *chunk)
{
  if (hud == NULL || chunk == NULL || hud->send_fn == NULL) {
    return false;
  }

  if ((chunk->flags & 0x04U) != 0U) {
    hud->media_fill      = 0U;
    hud->media_chunk_idx = 0U;
    return send_hud_pkt(hud, HUD_CMD_MEDIA_ERASE, NULL, 0U);
  }

  if ((chunk->flags & 0x01U) != 0U) {
    hud->media_slot         = (uint8_t)((chunk->flags >> 4U) & 0x0FU);
    hud->media_frame_idx    = hud->media_slot;
    hud->media_total_frames = 1U;
    hud->media_delay_units  = 0U;
    hud->media_chunk_idx    = 0U;
    hud->media_fill         = 0U;
    memset(hud->media_buf, 0, sizeof(hud->media_buf));

    uint8_t prep[12];
    memset(prep, 0, sizeof(prep));
    prep[0] = 0xA5U;
    if (chunk->seq == 0xFFFFU && chunk->len >= 10U) {
      hud->media_frame_idx    = chunk->data[0];
      hud->media_total_frames = (chunk->data[1] > 0U) ? chunk->data[1] : 1U;
      hud->media_delay_units  = chunk->data[2];
      prep[1]                 = hud->media_frame_idx;
      prep[2]                 = hud->media_total_frames;
      prep[3]                 = hud->media_delay_units;
      prep[4]                 = chunk->data[3];
      prep[5]                 = chunk->data[4];
      prep[10]                = chunk->data[7];
      prep[11]                = chunk->data[9];
      return send_hud_pkt(hud, HUD_CMD_MEDIA_PREP, prep, (uint16_t)sizeof(prep));
    }

    if (chunk->seq == 0U) {
      prep[1]  = hud->media_frame_idx;
      prep[2]  = 1U;
      prep[5]  = 0x80U; /* 32768 B (128x128x2) default static frame */
      prep[10] = 128U;
      prep[11] = 128U;
      (void)send_hud_pkt(hud, HUD_CMD_MEDIA_PREP, prep, (uint16_t)sizeof(prep));
    }
  }

  uint8_t len    = (chunk->len <= 24U) ? chunk->len : 24U;
  uint8_t offset = 0U;
  while (offset < len) {
    uint8_t space = (uint8_t)(56U - hud->media_fill);
    uint8_t avail = (uint8_t)(len - offset);
    uint8_t take  = (avail < space) ? avail : space;
    memcpy(&hud->media_buf[8U + hud->media_fill], &chunk->data[offset], take);
    hud->media_fill = (uint8_t)(hud->media_fill + take);
    offset          = (uint8_t)(offset + take);
    if (hud->media_fill >= 56U) {
      if (!send_media_chunk_frame(hud, 56U)) {
        return false;
      }
    }
  }

  if ((chunk->flags & 0x02U) != 0U && hud->media_fill > 0U) {
    return send_media_chunk_frame(hud, hud->media_fill);
  }
  return true;
}

static uint16_t exit_cursor_target(hud_engine_t *hud)
{
  if (hud->menu_return_cursor == HUD_MENU_PIC_VIEW) {
    hud->menu_pic_slot = hud->menu_return_pic_slot;
    return HUD_MENU_PIC_VIEW;
  }
  return HUD_MENU_HOME;
}

bool hud_engine_handle_rx(hud_engine_t *hud, uart7_rx_frame_t const *frame)
{
  if (hud == NULL || hud->cfg == NULL || frame == NULL) {
    return false;
  }

  hud->rx_count++;
  hud->last_rx_cmd = frame->cmd;
  hud->last_rx_len = frame->payload_len;
  uint8_t copy_len = (frame->payload_len < (uint8_t)sizeof(hud->last_rx_payload))
                       ? frame->payload_len
                       : (uint8_t)sizeof(hud->last_rx_payload);
  memset(hud->last_rx_payload, 0, sizeof(hud->last_rx_payload));
  if (copy_len > 0U) {
    memcpy(hud->last_rx_payload, frame->payload, copy_len);
  }

  /* TFT sends periodic cmd=0x81 status/heartbeat frames; sync cursor/slots without clobbering RGB cfg. */
  if (frame->cmd == 0x82U && frame->payload_len >= 2U && frame->payload[0] == 1U) {
    hud->cfg->tft_page = (frame->payload[1] < HUD_MAX_TFT_PAGE) ? frame->payload[1] : 0U;
    hud->options_dirty = true;
    return true;
  }
  if (frame->cmd == 0x88U) {
    uint8_t n = (frame->payload_len < (uint8_t)sizeof(hud->info_payload))
                  ? frame->payload_len
                  : (uint8_t)sizeof(hud->info_payload);
    hud->info_len = n;
    memset(hud->info_payload, 0, sizeof(hud->info_payload));
    if (n > 0U) {
      memcpy(hud->info_payload, frame->payload, n);
    }
    return false;
  }
  if ((frame->cmd == 0x81U || frame->cmd == 0x82U || frame->cmd == 0x85U) &&
      frame->payload_len >= 10U && frame->payload[0] == 0U) {
    /* Stock FUN_08012e80 UART7 RX offsets from 'D': [3]=p[0], [4..5]=p[1..2] cursor,
     * [6]=p[3] pic_slot, [7..12]=p[4..9] pic_exists[0..5], [13..14]=p[10..11] return state. */
    uint16_t rx_cursor = (uint16_t)frame->payload[1] | ((uint16_t)frame->payload[2] << 8U);
    uint8_t  rx_slot   = (frame->payload[3] < HUD_MAX_PIC_SLOTS) ? frame->payload[3] : 0U;
    for (uint8_t i = 0U; i < HUD_MAX_PIC_SLOTS; i++) {
      hud->menu_pic_exists[i] = frame->payload[4U + i];
    }
    /* Prevent queued stale echoes from clobbering an active transition, and reject unsolicited boot menus. */
    bool rx_is_menu  = (rx_cursor != HUD_MENU_HOME) && (rx_cursor != HUD_MENU_PIC_VIEW);
    bool rx_matches  = (rx_cursor == hud->menu_cursor);
    bool sync_active = hud->menu_dirty || (hud->menu_sync_ticks > 0U);
    if (!sync_active && rx_is_menu && !hud_engine_is_menu_open(hud)) {
      /* Reject stale TFT boot cursor (e.g. 0x0015) and push TFT back to Home/PicView. */
      if (frame->payload_len >= 12U &&
          (frame->payload[10] == HUD_MENU_HOME || frame->payload[10] == HUD_MENU_PIC_VIEW)) {
        hud->menu_return_cursor   = frame->payload[10];
        hud->menu_return_pic_slot = (frame->payload[11] < HUD_MAX_PIC_SLOTS) ? frame->payload[11] : 0U;
      }
      hud->menu_trigger    = 0U;
      hud->menu_lang_dirty = 0U;
      hud->menu_cursor     = exit_cursor_target(hud);
      hud->menu_dirty      = true;
      hud->status_dirty    = true;
      hud->menu_sync_ticks = 40U;
    } else if (!sync_active || rx_matches ||
               (frame->cmd == 0x85U && !hud_engine_is_menu_open(hud) && !rx_is_menu)) {
      hud->menu_trigger    = 0U;
      hud->menu_lang_dirty = 0U;
      if (rx_cursor == HUD_MENU_HOME || rx_cursor == HUD_MENU_PIC_VIEW) {
        hud->menu_return_cursor   = rx_cursor;
        hud->menu_return_pic_slot = rx_slot;
      } else if (frame->payload_len >= 12U &&
                 (frame->payload[10] == HUD_MENU_HOME || frame->payload[10] == HUD_MENU_PIC_VIEW)) {
        hud->menu_return_cursor   = frame->payload[10];
        hud->menu_return_pic_slot = (frame->payload[11] < HUD_MAX_PIC_SLOTS) ? frame->payload[11] : 0U;
      }
      if (rx_matches && !hud->menu_dirty) {
        hud->menu_sync_ticks = 0U;
      }
      hud->menu_cursor   = rx_cursor;
      hud->menu_pic_slot = rx_slot;
    }
    return false;
  }
  return false;
}

static bool find_any_pic_slot(uint8_t const exists[HUD_MAX_PIC_SLOTS], uint8_t start,
                              int8_t dir, uint8_t *out_slot)
{
  for (uint8_t n = 1U; n <= HUD_MAX_PIC_SLOTS; n++) {
    int32_t idx = ((int32_t)start + (int32_t)dir * (int32_t)n + 60) % (int32_t)HUD_MAX_PIC_SLOTS;
    if (exists[idx] != 0U) {
      *out_slot = (uint8_t)idx;
      return true;
    }
  }
  return false;
}

bool hud_engine_is_menu_open(hud_engine_t const *hud)
{
  return (hud != NULL) &&
         (hud->menu_cursor != HUD_MENU_HOME) &&
         (hud->menu_cursor != HUD_MENU_PIC_VIEW);
}

void hud_engine_close_menu(hud_engine_t *hud)
{
  if (!hud_engine_is_menu_open(hud)) {
    return;
  }
  hud->menu_cursor     = exit_cursor_target(hud);
  hud->menu_dirty      = true;
  hud->status_dirty    = true;
  hud->menu_sync_ticks = 40U;
}

static uint16_t cycle_top_menu(hud_engine_t *hud, uint16_t cur, hud_menu_act_t act)
{
  static uint16_t const k_top[6] = {
    HUD_MENU_PIC_MENU, HUD_MENU_SIDE_ROOT, HUD_MENU_RGB_ROOT,
    HUD_MENU_OS_ROOT, HUD_MENU_LANG_ROOT, HUD_MENU_INFO_ROOT,
  };
  int32_t idx = 0;
  for (int32_t i = 0; i < 6; i++) {
    if (k_top[i] == cur) {
      idx = i;
      break;
    }
  }
  if (act == HUD_MENU_ACT_NEXT) {
    return (idx >= 5) ? exit_cursor_target(hud) : k_top[idx + 1];
  }
  if (act == HUD_MENU_ACT_PREV) {
    return (idx <= 0) ? exit_cursor_target(hud) : k_top[idx - 1];
  }
  return cur;
}

static uint16_t cycle_submenu_row(uint16_t cur, uint16_t base_root, hud_menu_act_t act)
{
  uint8_t row = (uint8_t)(cur >> 4U); /* 1..5 */
  if (row < 1U || row > 5U) {
    row = 1U;
  }
  if (act == HUD_MENU_ACT_NEXT) {
    row = (row >= 5U) ? 1U : (uint8_t)(row + 1U);
  } else if (act == HUD_MENU_ACT_PREV) {
    row = (row <= 1U) ? 5U : (uint8_t)(row - 1U);
  }
  return (uint16_t)(((uint16_t)row << 4U) | (base_root & 0x0FU));
}

bool hud_engine_menu_input(hud_engine_t *hud, hud_menu_act_t act, uint32_t now_ms)
{
  if (hud == NULL || hud->cfg == NULL || act == HUD_MENU_ACT_NONE) {
    return false;
  }

  tk75_persist_cfg_t *c   = hud->cfg;
  bool                chg = false;
  uint16_t            cur = hud->menu_cursor;

  hud->menu_last_input_ms = now_ms;
  hud->menu_dirty         = true;
  hud->status_dirty       = true;
  hud->menu_sync_ticks    = 40U;

  if (cur == HUD_MENU_HOME) {
    hud->menu_return_cursor   = HUD_MENU_HOME;
    hud->menu_return_pic_slot = hud->menu_pic_slot;
    hud->menu_cursor = (act == HUD_MENU_ACT_PREV) ? HUD_MENU_INFO_ROOT : HUD_MENU_PIC_MENU;
    return false;
  }

  if (cur == HUD_MENU_PIC_VIEW) {
    uint8_t next_slot = hud->menu_pic_slot;
    if (act == HUD_MENU_ACT_OK) {
      hud->menu_return_cursor   = HUD_MENU_PIC_VIEW;
      hud->menu_return_pic_slot = hud->menu_pic_slot;
      hud->menu_cursor          = HUD_MENU_PIC_MENU;
    } else if (find_any_pic_slot(hud->menu_pic_exists, hud->menu_pic_slot,
                                 (act == HUD_MENU_ACT_NEXT) ? 1 : -1, &next_slot) &&
               next_slot != hud->menu_pic_slot) {
      hud->menu_pic_slot        = next_slot;
      hud->menu_return_cursor   = HUD_MENU_PIC_VIEW;
      hud->menu_return_pic_slot = next_slot;
      hud->menu_trigger         = 1U;
    } else {
      hud->menu_cursor        = HUD_MENU_HOME;
      hud->menu_return_cursor = HUD_MENU_HOME;
    }
    return false;
  }

  switch (cur) {
    case HUD_MENU_PIC_MENU:
    case HUD_MENU_SIDE_ROOT:
    case HUD_MENU_RGB_ROOT:
    case HUD_MENU_OS_ROOT:
    case HUD_MENU_LANG_ROOT:
    case HUD_MENU_INFO_ROOT:
      if (act == HUD_MENU_ACT_OK) {
        if (cur == HUD_MENU_PIC_MENU) {
          uint8_t slot = hud->menu_pic_slot;
          if (hud->menu_return_cursor != HUD_MENU_PIC_VIEW &&
              (hud->menu_pic_exists[slot] != 0U ||
               find_any_pic_slot(hud->menu_pic_exists, slot, 1, &slot))) {
            hud->menu_pic_slot        = slot;
            hud->menu_cursor          = HUD_MENU_PIC_VIEW;
            hud->menu_return_cursor   = HUD_MENU_PIC_VIEW;
            hud->menu_return_pic_slot = slot;
            hud->menu_trigger         = 1U;
          } else {
            /* Match stock 0x0011 submenu when no picture is active rather than exiting menu. */
            hud->menu_cursor = HUD_MENU_PIC_SUB;
          }
        } else {
          hud->menu_cursor = (uint16_t)(0x0010U | cur);
        }
      } else {
        hud->menu_cursor = cycle_top_menu(hud, cur, act);
      }
      break;

    case HUD_MENU_PIC_SUB:
      if (act == HUD_MENU_ACT_OK) {
        uint8_t slot = hud->menu_pic_slot;
        if (hud->menu_pic_exists[slot] != 0U ||
            find_any_pic_slot(hud->menu_pic_exists, slot, 1, &slot)) {
          hud->menu_pic_slot        = slot;
          hud->menu_cursor          = HUD_MENU_PIC_VIEW;
          hud->menu_return_cursor   = HUD_MENU_PIC_VIEW;
          hud->menu_return_pic_slot = slot;
          hud->menu_trigger         = 1U;
        } else {
          hud->menu_cursor = HUD_MENU_PIC_MENU;
        }
      } else {
        hud->menu_cursor = HUD_MENU_PIC_MENU;
      }
      break;

    case HUD_MENU_SIDE_FX:
    case HUD_MENU_SIDE_COLOR:
    case HUD_MENU_SIDE_SPEED:
    case HUD_MENU_SIDE_BRIGHT:
    case HUD_MENU_SIDE_BACK:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = (cur == HUD_MENU_SIDE_BACK) ? HUD_MENU_SIDE_ROOT : (uint16_t)(0x0100U | cur);
      } else {
        hud->menu_cursor = cycle_submenu_row(cur, HUD_MENU_SIDE_ROOT, act);
      }
      break;

    case HUD_MENU_SIDE_ED_FX:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_SIDE_FX;
      } else {
        c->side_effect  = adjust_wrap_u8(c->side_effect, HUD_MAX_SIDE_FX, act);
        c->side_enabled = (c->side_effect != 0U) ? 1U : 0U;
        chg             = true;
      }
      break;

    case HUD_MENU_SIDE_ED_CLR:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_SIDE_COLOR;
      } else {
        uint8_t idx = adjust_wrap_u8(color_idx_from_hs(c->side_hue, c->side_sat), HUD_MAX_TFT_COLOR, act);
        c->side_hue = k_tft_colors[idx].hue;
        c->side_sat = k_tft_colors[idx].sat;
        chg         = true;
      }
      break;

    case HUD_MENU_SIDE_ED_SPD:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_SIDE_SPEED;
      } else {
        uint8_t st = adjust_clamp_u8(quantize_u8_step(c->side_speed, HUD_SIDE_SPEED_MAX_STEP),
                                     HUD_SIDE_SPEED_MAX_STEP, act);
        c->side_speed = step_to_u8(st, HUD_SIDE_SPEED_MAX_STEP);
        chg           = true;
      }
      break;

    case HUD_MENU_SIDE_ED_BRT:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_SIDE_BRIGHT;
      } else {
        uint8_t st = adjust_clamp_u8(quantize_u8_step(c->side_brightness, HUD_SIDE_BRIGHT_MAX_STEP),
                                     HUD_SIDE_BRIGHT_MAX_STEP, act);
        c->side_brightness = step_to_u8(st, HUD_SIDE_BRIGHT_MAX_STEP);
        c->side_enabled    = (st > 0U && c->side_effect != 0U) ? 1U : 0U;
        chg                = true;
      }
      break;

    case HUD_MENU_RGB_FX:
    case HUD_MENU_RGB_COLOR:
    case HUD_MENU_RGB_BRIGHT:
    case HUD_MENU_RGB_SPEED:
    case HUD_MENU_RGB_BACK:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = (cur == HUD_MENU_RGB_BACK) ? HUD_MENU_RGB_ROOT : (uint16_t)(0x0100U | cur);
      } else {
        hud->menu_cursor = cycle_submenu_row(cur, HUD_MENU_RGB_ROOT, act);
      }
      break;

    case HUD_MENU_RGB_ED_FX:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_RGB_FX;
      } else {
        c->rgb_effect  = adjust_wrap_u8(c->rgb_effect, HUD_MAX_RGB_FX, act);
        c->rgb_enabled = (c->rgb_effect != 0U) ? 1U : 0U;
        chg            = true;
      }
      break;

    case HUD_MENU_RGB_ED_CLR:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_RGB_COLOR;
      } else {
        uint8_t idx = adjust_wrap_u8(color_idx_from_hs(c->rgb_hue, c->rgb_sat), HUD_MAX_TFT_COLOR, act);
        c->rgb_hue = k_tft_colors[idx].hue;
        c->rgb_sat = k_tft_colors[idx].sat;
        chg        = true;
      }
      break;

    case HUD_MENU_RGB_ED_BRT:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_RGB_BRIGHT;
      } else {
        uint8_t st = adjust_clamp_u8(rgb_brightness_step(c->rgb_brightness), HUD_RGB_BRIGHT_MAX_STEP, act);
        c->rgb_brightness = step_to_u8(st, HUD_RGB_BRIGHT_MAX_STEP);
        c->rgb_enabled    = (st > 0U && c->rgb_effect != 0U) ? 1U : 0U;
        chg               = true;
      }
      break;

    case HUD_MENU_RGB_ED_SPD:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_RGB_SPEED;
      } else {
        uint8_t st = adjust_clamp_u8(quantize_u8_step(c->rgb_speed, HUD_RGB_SPEED_MAX_STEP),
                                     HUD_RGB_SPEED_MAX_STEP, act);
        c->rgb_speed = step_to_u8(st, HUD_RGB_SPEED_MAX_STEP);
        chg          = true;
      }
      break;

    case HUD_MENU_OS:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_OS_ROOT;
      } else {
        c->mac_mode          = (c->mac_mode != 0U) ? 0U : 1U;
        hud->status.mac_mode = (c->mac_mode != 0U);
        hud->options_dirty   = true;
        hud->menu_trigger    = 1U;
        chg                  = true;
      }
      break;

    case HUD_MENU_LANG:
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_LANG_ROOT;
      } else {
        hud->menu_lang_dirty = (hud->menu_lang_dirty != 0U) ? 0U : 1U;
        hud->menu_trigger    = 1U;
      }
      break;

    case HUD_MENU_INFO:
      /* Stock FUN_080159d8 cursor 0x0017: TFT screen brightness control.
       * PREV (CCW) toggles +0x20 (cmd=0x01 p[4]), NEXT (CW) toggles +0x1f (cmd=0x01 p[5]). */
      if (act == HUD_MENU_ACT_OK) {
        hud->menu_cursor = HUD_MENU_INFO_ROOT;
      } else if (act == HUD_MENU_ACT_PREV) {
        hud->menu_tft_brt_dn = (uint8_t)((hud->sent_tft_brt_dn ^ 1U) & 1U);
      } else if (act == HUD_MENU_ACT_NEXT) {
        hud->menu_tft_brt_up = (uint8_t)((hud->sent_tft_brt_up ^ 1U) & 1U);
      }
      break;

    default:
      hud->menu_cursor = HUD_MENU_HOME;
      break;
  }

  return chg;
}

static void advance_rtc_if_valid(hud_engine_t *hud, uint32_t now_ms)
{
  if (!hud->rtc_valid) {
    return;
  }
  if (!hud->rtc_tick_primed) {
    hud->last_rtc_tick_ms = now_ms;
    hud->rtc_tick_primed  = true;
  }
  while ((uint32_t)(now_ms - hud->last_rtc_tick_ms) >= 1000U) {
    hud->last_rtc_tick_ms += 1000U;
    advance_rtc_one_second(&hud->rtc);
  }
}

void hud_engine_update_wpm(hud_engine_t *hud, uint32_t now_ms)
{
  if (hud != NULL) {
    update_wpm_buckets(hud, now_ms);
    advance_rtc_if_valid(hud, now_ms);
  }
}

void hud_engine_tick(hud_engine_t *hud, uint32_t now_ms)
{
  if (hud == NULL || hud->cfg == NULL || hud->send_fn == NULL) {
    return;
  }

  update_wpm_buckets(hud, now_ms);
  advance_rtc_if_valid(hud, now_ms);

  if (hud_engine_is_menu_open(hud) &&
      (uint32_t)(now_ms - hud->menu_last_input_ms) >= HUD_MENU_TIMEOUT_MS) {
    hud_engine_close_menu(hud);
  }

  if (!hud->menu_dirty && hud->menu_sync_ticks > 0U) {
    hud->menu_sync_ticks--;
  }

  if (hud->rtc_dirty) {
    if (send_rtc_packet(hud)) {
      hud->rtc_dirty = false;
    }
    return;
  }

  if (hud->menu_dirty) {
    if (send_status_packet(hud)) {
      hud->menu_dirty     = false;
      hud->status_dirty   = false;
      hud->last_status_ms = now_ms;
    }
    return;
  }

  if (hud->options_dirty) {
    if (send_options_packet(hud)) {
      hud->options_dirty = false;
    }
    return;
  }

  if (hud->status_dirty && (uint32_t)(now_ms - hud->last_status_ms) >= HUD_STATUS_INTERVAL_MS) {
    if (send_status_packet(hud)) {
      hud->status_dirty   = false;
      hud->last_status_ms = now_ms;
    }
    return;
  }

  if (!hud->wake_ready_sent) {
    if (send_hud_pkt(hud, HUD_CMD_WAKE_READY, NULL, 0U)) {
      hud->wake_ready_sent   = true;
      hud->last_heartbeat_ms = now_ms;
    }
    return;
  }

  if ((uint32_t)(now_ms - hud->last_heartbeat_ms) >= HUD_HEARTBEAT_INTERVAL_MS) {
    uint8_t poll_cmd = (hud->info_len == 0U) ? HUD_CMD_WAKE_READY : HUD_CMD_HEARTBEAT;
    if (send_hud_pkt(hud, poll_cmd, NULL, 0U)) {
      hud->last_heartbeat_ms = now_ms;
    }
  }
}
