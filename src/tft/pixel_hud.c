#include "pixel_hud.h"
#include "macro_player.h"
#include "macro_recorder.h"
#include "rgb_effects.h"
#include <string.h>

/* Minimal Pixel-Art Palette (RGB565) */
#define COL_BG      0x0862U /* #0B0F17 Deep Obsidian */
#define COL_CARD    0x10E5U /* #151C2C Dark Slate */
#define COL_BORDER  0x29AAU /* #2A3550 Crisp Pixel Bevel */
#define COL_DIM     0x2127U /* #202638 Cavity / Slider Track */
#define COL_CYAN    0x3F5FU /* #38E8FF Primary Accent */
#define COL_MINT    0x37D3U /* #36F998 Actuated / Good / Active */
#define COL_AMBER   0xFDC6U /* #FFB830 Edit / Rapid Trigger / Warm */
#define COL_CORAL   0xF9C9U /* #FF3B4B Alert / Low Battery */
#define COL_MUTED   0x63B1U /* #64748B Muted Label */
#define COL_WHITE   0xFFDFU /* #F8FAFC Crisp White */
#define COL_KEYCAP  0x4A8EU /* #4B5570 Switch Keycap Body */
#define COL_KEYTOP  0x8474U /* #858FA4 Switch Keycap Highlight */
#define COL_SPRING  0x94D6U /* #949DB0 Switch Coil Spring */
#define COL_HOUSING 0x31A9U /* #32364A Switch Housing Wall */

#define PROBE_INTERVAL_MS   100U
#define ACK_TIMEOUT_MS      150U
#define ANIM_PERIOD_MS      28U
#define MENU_TIMEOUT_MS     15000U
#define SCREENSAVER_IDLE_MS 30000U
#define PLUNGER_MAX_PX      18U
#define MAX_BATCH_RECTS     15U

typedef struct {
  uint8_t  count;
  uint8_t  buf[2U + MAX_BATCH_RECTS * 6U];
} rect_batch_t;

static bool enqueue_op(pixel_hud_t *hud, uint8_t cmd, uint8_t const *data, uint8_t len)
{
  if (hud->q_count >= PIXEL_HUD_QUEUE_CAP || len > UART7_MAX_PAYLOAD) {
    return false;
  }
  pixel_hud_op_t *slot = &hud->q[hud->q_tail];
  slot->cmd = cmd;
  slot->len = len;
  if (len > 0U && data != NULL) {
    memcpy(slot->data, data, len);
  }
  hud->q_tail = (uint8_t)((hud->q_tail + 1U) % PIXEL_HUD_QUEUE_CAP);
  hud->q_count++;
  return true;
}

static void batch_init(rect_batch_t *b)
{
  b->count  = 0U;
  b->buf[0] = 0U;
  b->buf[1] = 0U;
}

static void batch_add(rect_batch_t *b, uint8_t x, uint8_t y, uint8_t w, uint8_t h, uint16_t col)
{
  if (b->count >= MAX_BATCH_RECTS || w == 0U || h == 0U) {
    return;
  }
  uint8_t off = (uint8_t)(2U + b->count * 6U);
  b->buf[off + 0U] = x;
  b->buf[off + 1U] = y;
  b->buf[off + 2U] = w;
  b->buf[off + 3U] = h;
  b->buf[off + 4U] = (uint8_t)(col >> 8U);
  b->buf[off + 5U] = (uint8_t)(col & 0xFFU);
  b->count++;
  b->buf[1] = b->count;
}

static void batch_submit(pixel_hud_t *hud, rect_batch_t *b, bool flush)
{
  if (b->count == 0U) {
    return;
  }
  b->buf[0] = flush ? 1U : 0U;
  (void)enqueue_op(hud, PIXEL_HUD_CMD_BATCH_RECT, b->buf, (uint8_t)(2U + b->count * 6U));
  batch_init(b);
}

static void q_fill(pixel_hud_t *hud, uint8_t x, uint8_t y, uint8_t w, uint8_t h,
                   uint16_t col, bool flush)
{
  uint8_t p[7] = {
    x, y, w, h, (uint8_t)(col >> 8U), (uint8_t)(col & 0xFFU), flush ? 1U : 0U,
  };
  (void)enqueue_op(hud, PIXEL_HUD_CMD_FILL_RECT, p, (uint8_t)sizeof(p));
}

static void q_text(pixel_hud_t *hud, uint8_t x, uint8_t y, char const *txt,
                   uint16_t fg, uint16_t bg, uint8_t scale, bool transp, bool flush)
{
  uint8_t p[72];
  size_t  n = strlen(txt);
  if (n > 24U) {
    n = 24U;
  }
  p[0] = x;
  p[1] = y;
  p[2] = (uint8_t)(fg >> 8U);
  p[3] = (uint8_t)(fg & 0xFFU);
  p[4] = (uint8_t)(bg >> 8U);
  p[5] = (uint8_t)(bg & 0xFFU);
  p[6] = (uint8_t)((scale & 3U) | (transp ? 0x40U : 0U) | (flush ? 0x80U : 0U));
  memcpy(&p[7], txt, n);
  (void)enqueue_op(hud, PIXEL_HUD_CMD_TEXT, p, (uint8_t)(7U + n));
}

static void q_brightness_pct(pixel_hud_t *hud, uint8_t pct)
{
  uint8_t p[2] = { (pct <= 100U) ? pct : 100U, 1U };
  (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, p, 2U);
}

static void fmt_u8_pct(uint8_t pct, char out[5])
{
  if (pct >= 100U) {
    out[0] = '1'; out[1] = '0'; out[2] = '0'; out[3] = '%'; out[4] = '\0';
  } else if (pct >= 10U) {
    out[0] = ' ';
    out[1] = (char)('0' + (pct / 10U));
    out[2] = (char)('0' + (pct % 10U));
    out[3] = '%';
    out[4] = '\0';
  } else {
    out[0] = ' '; out[1] = ' ';
    out[2] = (char)('0' + pct);
    out[3] = '%'; out[4] = '\0';
  }
}

static void fmt_mm_tenths(uint16_t tenths, char out[6])
{
  if (tenths > 99U) {
    tenths = 99U;
  }
  out[0] = (char)('0' + (tenths / 10U));
  out[1] = '.';
  out[2] = (char)('0' + (tenths % 10U));
  out[3] = 'm';
  out[4] = 'm';
  out[5] = '\0';
}

static void fmt_wpm3(uint16_t wpm, char out[4])
{
  if (wpm > 999U) {
    wpm = 999U;
  }
  out[0] = (wpm >= 100U) ? (char)('0' + ((wpm / 100U) % 10U)) : ' ';
  out[1] = (wpm >= 10U)  ? (char)('0' + ((wpm / 10U) % 10U))  : ' ';
  out[2] = (char)('0' + (wpm % 10U));
  out[3] = '\0';
}

static uint8_t u8_to_pct10(uint8_t val, uint8_t enabled)
{
  if (enabled == 0U || val == 0U) {
    return 0U;
  }
  uint32_t pct = (((uint32_t)val * 100U) + 127U) / 255U;
  uint32_t rounded = ((pct + 5U) / 10U) * 10U;
  if (rounded == 0U) {
    rounded = 10U;
  }
  return (rounded > 100U) ? 100U : (uint8_t)rounded;
}

static uint8_t pct10_to_u8(uint8_t pct)
{
  if (pct == 0U) {
    return 0U;
  }
  if (pct >= 100U) {
    return 255U;
  }
  return (uint8_t)(((uint32_t)pct * 255U + 50U) / 100U);
}

static uint32_t compute_header_hash(pixel_hud_t const *hud)
{
  pixel_hud_live_t const *l = &hud->live;
  uint32_t h = 2166136261U;
  h = (h ^ l->conn_mode) * 16777619U;
  h = (h ^ l->bt_slot) * 16777619U;
  h = (h ^ l->active_layer) * 16777619U;
  h = (h ^ (l->charging ? 1U : 0U)) * 16777619U;
  h = (h ^ (l->vbus_present ? 1U : 0U)) * 16777619U;
  h = (h ^ (l->macro_recording ? (uint32_t)(0xA5U | ((uint32_t)l->macro_rec_slot << 8U)) : 0U)) * 16777619U;
  h = (h ^ l->macro_speed_toast) * 16777619U;
  h = (h ^ l->battery_pct) * 16777619U;
  return h;
}

/* Draw pixel-art power-state indicator + battery icon (fits <= 12 rects in 1 atomic batch):
 *   - USB Charging  (vbus && charging):  Amber/White lightning bolt + Amber battery outline/fill
 *   - USB Full/Ext  (vbus && !charging): Cyan USB plug icon + Cyan outline + Mint fill
 *   - Battery Mode  (!vbus):             Clean battery icon (no bolt/plug), Mint (or Coral <=20%) */
static uint16_t batch_add_battery_widget(rect_batch_t *b, uint8_t y0, pixel_hud_live_t const *l)
{
  bool     usb_chg  = l->vbus_present && l->charging;
  bool     usb_full = l->vbus_present && !l->charging;
  uint16_t batt_col = (l->battery_pct <= 20U && !usb_chg) ? COL_CORAL
                    : (usb_chg ? COL_AMBER : COL_MINT);
  uint16_t frame_col = usb_chg ? COL_AMBER : (usb_full ? COL_CYAN : COL_MUTED);

  if (usb_chg) {
    /* Pixel-art lightning bolt (x=66..71, y=y0..y0+9) */
    batch_add(b, 69U, (uint8_t)(y0 + 0U), 3U, 2U, COL_AMBER);
    batch_add(b, 67U, (uint8_t)(y0 + 2U), 4U, 2U, COL_WHITE);
    batch_add(b, 66U, (uint8_t)(y0 + 4U), 6U, 2U, COL_AMBER);
    batch_add(b, 68U, (uint8_t)(y0 + 6U), 4U, 2U, COL_WHITE);
    batch_add(b, 67U, (uint8_t)(y0 + 8U), 3U, 2U, COL_AMBER);
  } else if (usb_full) {
    /* Pixel-art USB power plug icon (x=64..72, y=y0+2..y0+7) */
    batch_add(b, 64U, (uint8_t)(y0 + 4U), 2U, 2U, COL_CYAN);
    batch_add(b, 66U, (uint8_t)(y0 + 2U), 4U, 6U, COL_CYAN);
    batch_add(b, 70U, (uint8_t)(y0 + 3U), 3U, 1U, COL_WHITE);
    batch_add(b, 70U, (uint8_t)(y0 + 6U), 3U, 1U, COL_WHITE);
    batch_add(b, 67U, (uint8_t)(y0 + 4U), 2U, 2U, COL_WHITE);
  }

  /* Pixel-art battery casing @ (75, y0), 17x10 */
  batch_add(b, 75U, y0, 15U, 1U, frame_col);                 /* top */
  batch_add(b, 75U, (uint8_t)(y0 + 9U), 15U, 1U, frame_col); /* bottom */
  batch_add(b, 75U, y0, 1U, 10U, frame_col);                 /* left */
  batch_add(b, 89U, y0, 1U, 10U, frame_col);                 /* right */
  batch_add(b, 90U, (uint8_t)(y0 + 3U), 2U, 4U, frame_col);  /* tip */
  batch_add(b, 77U, (uint8_t)(y0 + 2U), 11U, 6U, COL_DIM);   /* empty interior */
  uint8_t fill_w = (uint8_t)(((uint32_t)l->battery_pct * 11U + 50U) / 100U);
  if (fill_w > 11U) fill_w = 11U;
  if (fill_w > 0U) {
    batch_add(b, 77U, (uint8_t)(y0 + 2U), fill_w, 6U, batt_col);
  }
  return batt_col;
}

static uint8_t compute_badges_hash(pixel_hud_t const *hud)
{
  tk75_persist_cfg_t const *c = hud->cfg;
  pixel_hud_live_t const   *l = &hud->live;
  return (uint8_t)((c->hall_rt_enabled ? 1U : 0U) |
                   ((c->hall_socd_mode & 3U) << 1U) |
                   (((l->host_leds & 2U) != 0U) ? 8U : 0U) |
                   (c->mac_mode ? 16U : 0U) |
                   (c->win_lock ? 32U : 0U));
}

/* Draw top status bar (y = 0..19) */
static void draw_home_header(pixel_hud_t *hud)
{
  pixel_hud_live_t const *l = &hud->live;
  rect_batch_t b;
  batch_init(&b);

  /* Header background + 1px bottom border */
  batch_add(&b, 0U, 0U, 128U, 18U, COL_CARD);
  batch_add(&b, 0U, 18U, 128U, 1U, COL_BORDER);

  /* Pixel-art power indicator (USB charging bolt / USB full plug / battery) + battery icon */
  uint16_t batt_col = batch_add_battery_widget(&b, 4U, l);
  batch_submit(hud, &b, false);

  if (l->macro_recording) {
    char rec_str[7] = { 'R', 'E', 'C', ' ', 'M', (char)('0' + (l->macro_rec_slot % 10U)), '\0' };
    q_text(hud, 4U, 3U, rec_str, COL_CORAL, COL_CARD, 1U, false, false);
  } else if (l->macro_speed_toast != 0U) {
    q_text(hud, 4U, 3U, macro_player_speed_label(l->macro_speed_toast), COL_AMBER, COL_CARD, 1U, false, false);
  } else {
    char mode_str[4] = "USB";
    uint16_t mode_col = COL_CYAN;
    if (l->conn_mode == 1U) {
      mode_str[0] = '2'; mode_str[1] = '.'; mode_str[2] = '4';
      mode_col = COL_MINT;
    } else if (l->conn_mode == 2U) {
      mode_str[0] = 'B'; mode_str[1] = 'T'; mode_str[2] = (char)('1' + (l->bt_slot % 3U));
      mode_col = COL_AMBER;
    }
    q_text(hud, 4U, 3U, mode_str, mode_col, COL_CARD, 1U, false, false);

    char lay_str[3] = { 'L', (char)('0' + (l->active_layer & 3U)), '\0' };
    q_text(hud, 28U, 3U, lay_str, COL_WHITE, COL_CARD, 1U, false, false);
  }

  char bstr[5];
  fmt_u8_pct(l->battery_pct, bstr);
  q_text(hud, 95U, 3U, bstr, batt_col, COL_CARD, 1U, false, false);

  hud->prev_header_hash = compute_header_hash(hud);
}

/* Draw pixel-art animated Hall-effect switch inside left card (x=4..57, y=23..107)
 * Packed into 1 single atomic 0x26 batch packet for silky-smooth, tear-free motion! */
static void draw_switch_animation(pixel_hud_t *hud, uint8_t dy, bool actuated, bool flush)
{
  if (dy > PLUNGER_MAX_PX) {
    dy = PLUNGER_MAX_PX;
  }
  uint8_t act_px = (uint8_t)(((uint32_t)hud->cfg->hall_actuation_um * PLUNGER_MAX_PX) / 4000U);
  if (act_px < 1U) act_px = 1U;
  if (act_px > PLUNGER_MAX_PX) act_px = PLUNGER_MAX_PX;

  uint16_t stem_col   = actuated ? COL_MINT : ((dy > 0U) ? COL_CYAN : COL_KEYTOP);
  uint16_t sensor_col = actuated ? COL_MINT : ((dy > 2U) ? COL_CYAN : COL_BORDER);
  uint16_t cap_top    = actuated ? COL_MINT : ((dy > 0U) ? COL_CYAN : COL_KEYTOP);

  rect_batch_t b;
  batch_init(&b);

  /* 1. Clear switch interior cleanly */
  batch_add(&b, 6U, 25U, 50U, 80U, COL_CARD);

  /* 2. Switch lower housing walls + dark cavity */
  batch_add(&b, 11U, 56U, 40U, 45U, COL_HOUSING);
  batch_add(&b, 15U, 59U, 32U, 38U, COL_DIM);

  /* 3. Actuation threshold horizontal marker line inside housing */
  uint8_t act_y = (uint8_t)(60U + act_px);
  batch_add(&b, 15U, act_y, 32U, 1U, actuated ? COL_MINT : COL_AMBER);

  /* 4. Pixel-art compressing coil spring (3 coils between stem bottom & sensor) */
  uint8_t spring_top = (uint8_t)(60U + dy);
  uint8_t spring_bot = 89U;
  if (spring_bot > spring_top + 4U) {
    uint8_t span = (uint8_t)(spring_bot - spring_top);
    batch_add(&b, 24U, (uint8_t)(spring_top + (span / 4U)), 14U, 2U, COL_SPRING);
    batch_add(&b, 22U, (uint8_t)(spring_top + (span / 2U)), 18U, 2U, COL_SPRING);
    batch_add(&b, 24U, (uint8_t)(spring_top + ((span * 3U) / 4U)), 14U, 2U, COL_SPRING);
  } else {
    batch_add(&b, 22U, 86U, 18U, 3U, COL_SPRING);
  }

  /* 5. Hall magnetic sensor block at bottom of switch cavity */
  batch_add(&b, 21U, 91U, 20U, 5U, sensor_col);

  /* 6. Moving keycap top bevel + body + cross-stem plunger */
  batch_add(&b, 13U, (uint8_t)(27U + dy), 36U, 4U, cap_top);    /* Keycap top highlight */
  batch_add(&b, 10U, (uint8_t)(31U + dy), 42U, 14U, COL_KEYCAP); /* Keycap main block */
  batch_add(&b, 14U, (uint8_t)(34U + dy), 6U, 2U, COL_WHITE);   /* Minimal pixel shine */
  batch_add(&b, 25U, (uint8_t)(45U + dy), 12U, 15U, stem_col);  /* Moving stem plunger */
  batch_add(&b, 28U, (uint8_t)(57U + dy), 6U, 3U, COL_WHITE);   /* Magnet tip */

  batch_submit(hud, &b, flush);
  hud->anim_plunger_px = dy;
  hud->prev_actuated   = actuated;
}

static uint16_t update_display_travel(pixel_hud_t *hud)
{
  uint16_t raw = hud->live.max_travel_um;
  if (hud->live.any_actuated || raw >= 200U) {
    hud->travel_active = true;
  } else if (raw < 120U) {
    hud->travel_active = false;
  }

  if (!hud->travel_active) {
    hud->smooth_travel_um = 0U;
    return 0U;
  }

  /* Light 1/2 EMA smoothing for crisp, jitter-free TFT animation */
  if (hud->smooth_travel_um == 0U) {
    hud->smooth_travel_um = raw;
  } else {
    hud->smooth_travel_um = (uint16_t)(((uint32_t)hud->smooth_travel_um + (uint32_t)raw + 1U) >> 1U);
  }
  return hud->smooth_travel_um;
}

static void draw_home_right_stats(pixel_hud_t *hud, bool force_all, bool flush)
{
  uint16_t wpm = hud->live.wpm;
  uint16_t disp_travel = hud->travel_active ? hud->smooth_travel_um : 0U;
  uint16_t travel_t = (disp_travel > 0U)
                        ? (uint16_t)((disp_travel + 50U) / 100U)
                        : (uint16_t)((hud->cfg->hall_actuation_um + 50U) / 100U);

  if (force_all || wpm != hud->prev_wpm) {
    char wbuf[4];
    fmt_wpm3(wpm, wbuf);
    q_text(hud, 64U, 26U, wbuf, COL_WHITE, COL_BG, 2U, false, false);
    if (force_all) {
      q_text(hud, 104U, 38U, "WPM", COL_MUTED, COL_BG, 1U, false, false);
    }
    hud->prev_wpm = wpm;
  }

  if (force_all || travel_t != hud->prev_travel_disp || hud->live.any_actuated != hud->prev_actuated) {
    char mm[6];
    fmt_mm_tenths(travel_t, mm);
    uint16_t tcol = hud->live.any_actuated ? COL_MINT
                  : ((disp_travel > 0U) ? COL_CYAN : COL_MUTED);
    q_text(hud, 64U, 58U, mm, tcol, COL_BG, 1U, false, false);

    /* Compact horizontal live travel bar (x=64..122, y=71..80: clear full marker height first) */
    rect_batch_t b;
    batch_init(&b);
    batch_add(&b, 64U, 71U, 59U, 10U, COL_BG);
    batch_add(&b, 64U, 73U, 58U, 6U, COL_DIM);
    uint8_t fill_w = (uint8_t)(((uint32_t)disp_travel * 58U) / 4000U);
    if (fill_w > 58U) fill_w = 58U;
    if (fill_w > 0U) {
      batch_add(&b, 64U, 73U, fill_w, 6U, tcol);
    }
    uint8_t act_x = (uint8_t)(64U + (((uint32_t)hud->cfg->hall_actuation_um * 58U) / 4000U));
    if (act_x > 121U) act_x = 121U;
    batch_add(&b, act_x, 71U, 2U, 10U, COL_AMBER);
    batch_submit(hud, &b, false);
    hud->prev_travel_disp = travel_t;
  }

  uint8_t bhash = compute_badges_hash(hud);
  if (force_all || bhash != hud->prev_badges_hash) {
    bool rt_on   = (hud->cfg->hall_rt_enabled != 0U);
    bool caps_on = ((hud->live.host_leds & 2U) != 0U);
    rect_batch_t b;
    batch_init(&b);
    batch_add(&b, 64U, 88U, 26U, 16U, rt_on ? COL_AMBER : COL_CARD);
    batch_add(&b, 94U, 88U, 28U, 16U, caps_on ? COL_CORAL : COL_CARD);
    batch_submit(hud, &b, false);

    q_text(hud, 71U, 90U, "RT", rt_on ? COL_BG : COL_MUTED,
           rt_on ? COL_AMBER : COL_CARD, 1U, true, false);
    char const *b2 = caps_on ? "CAP" : (hud->cfg->mac_mode ? "MAC" : "WIN");
    q_text(hud, 99U, 90U, b2, caps_on ? COL_WHITE : COL_CYAN,
           caps_on ? COL_CORAL : COL_CARD, 1U, true, flush);
    hud->prev_badges_hash = bhash;
  } else if (flush) {
    uint8_t f = 0xFFU;
    (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, &f, 1U);
  }
}

/* Bottom page-indicator strip (y = 112..127) */
static void draw_footer(pixel_hud_t *hud, bool flush)
{
  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 0U, 111U, 128U, 1U, COL_BORDER);
  batch_add(&b, 0U, 112U, 128U, 16U, COL_CARD);

  /* 6 crisp pixel page indicator dots */
  uint8_t x = 6U;
  for (uint8_t i = 0U; i < PIXEL_PAGE_COUNT; i++) {
    if (i == hud->page) {
      batch_add(&b, x, 118U, 10U, 4U, COL_CYAN);
      x = (uint8_t)(x + 13U);
    } else {
      batch_add(&b, x, 118U, 4U, 4U, COL_MUTED);
      x = (uint8_t)(x + 7U);
    }
  }
  batch_submit(hud, &b, false);

  char const *hint = hud->menu_open ? (hud->editing ? "TURN:SET" : "OK:EDIT ") : "FN+KNOB ";
  uint16_t hcol    = hud->editing ? COL_AMBER : (hud->menu_open ? COL_CYAN : COL_MUTED);
  q_text(hud, 74U, 114U, hint, hcol, COL_CARD, 1U, false, flush);
}

/* Draw reusable menu top tab bar for pages 1..3 (y = 0..19) */
static void draw_menu_header(pixel_hud_t *hud, char const *title, uint8_t icon_kind)
{
  bool tab_focused = (hud->focus_row == 0U);
  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 0U, 0U, 128U, 18U, tab_focused ? COL_BORDER : COL_CARD);
  batch_add(&b, 0U, 18U, 128U, 1U, tab_focused ? COL_CYAN : COL_BORDER);

  /* Minimal 8x8 pixel-art page icon at (6, 5) */
  if (icon_kind == 0U) {
    /* Sun (Brightness) */
    batch_add(&b, 8U, 7U, 4U, 4U, COL_AMBER);
    batch_add(&b, 9U, 4U, 2U, 2U, COL_AMBER);
    batch_add(&b, 9U, 12U, 2U, 2U, COL_AMBER);
    batch_add(&b, 5U, 8U, 2U, 2U, COL_AMBER);
    batch_add(&b, 13U, 8U, 2U, 2U, COL_AMBER);
  } else if (icon_kind == 1U) {
    /* Switch stem (Hall Effect) */
    batch_add(&b, 6U, 5U, 8U, 3U, COL_CYAN);
    batch_add(&b, 8U, 8U, 4U, 5U, COL_MINT);
  } else if (icon_kind == 2U) {
    /* Play triangle (Macro) */
    batch_add(&b, 7U, 5U, 2U, 8U, COL_MINT);
    batch_add(&b, 9U, 6U, 2U, 6U, COL_MINT);
    batch_add(&b, 11U, 7U, 2U, 4U, COL_AMBER);
    batch_add(&b, 13U, 8U, 2U, 2U, COL_AMBER);
  } else if (icon_kind == 4U) {
    /* RGB Palette (Lighting / Colors & Effects) */
    batch_add(&b, 6U, 5U, 4U, 4U, COL_CORAL);
    batch_add(&b, 11U, 5U, 4U, 4U, COL_MINT);
    batch_add(&b, 8U, 10U, 4U, 4U, COL_CYAN);
  } else {
    /* Gear (System) */
    batch_add(&b, 7U, 6U, 6U, 6U, COL_CYAN);
    batch_add(&b, 9U, 8U, 2U, 2U, COL_CARD);
  }
  batch_submit(hud, &b, false);

  q_text(hud, 20U, 3U, title, tab_focused ? COL_CYAN : COL_WHITE,
         tab_focused ? COL_BORDER : COL_CARD, 1U, false, false);
  q_text(hud, 108U, 3U, "<>", tab_focused ? COL_AMBER : COL_MUTED,
         tab_focused ? COL_BORDER : COL_CARD, 1U, false, false);
}

/* Draw one compact pixel-art slider row (height 19 px, fits 4 rows cleanly on BRIGHTNESS page) */
static void draw_slider_row_compact(pixel_hud_t *hud, uint8_t row_idx, uint8_t y,
                                    char const *label, uint8_t pct, uint16_t bar_col)
{
  bool focused = (hud->focus_row == row_idx);
  bool editing = focused && hud->editing;
  uint16_t border = editing ? COL_AMBER : (focused ? COL_CYAN : COL_BORDER);
  uint16_t bg     = focused ? COL_CARD : COL_BG;

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 4U, y, 120U, 19U, border);
  batch_add(&b, 5U, (uint8_t)(y + 1U), 118U, 17U, bg);
  if (focused) {
    batch_add(&b, 5U, (uint8_t)(y + 1U), 3U, 17U, editing ? COL_AMBER : COL_CYAN);
  }
  /* Track @ (36, y+6, w=54, h=7) */
  batch_add(&b, 36U, (uint8_t)(y + 6U), 54U, 7U, COL_DIM);
  uint8_t fill_w = (uint8_t)(((uint32_t)pct * 54U + 50U) / 100U);
  if (fill_w > 54U) fill_w = 54U;
  if (fill_w > 0U) {
    batch_add(&b, 36U, (uint8_t)(y + 6U), fill_w, 7U, editing ? COL_AMBER : bar_col);
  }
  batch_submit(hud, &b, false);

  q_text(hud, 11U, (uint8_t)(y + 4U), label, focused ? COL_WHITE : COL_MUTED, bg, 1U, false, false);
  char pstr[5];
  fmt_u8_pct(pct, pstr);
  q_text(hud, 95U, (uint8_t)(y + 4U), pstr, editing ? COL_AMBER : COL_WHITE, bg, 1U, false, false);
}

static char const *caps_side_fx_short_name(uint8_t enabled, uint8_t fx)
{
  static char const *const k_names[CAPS_SIDE_FX_COUNT] = {
    "SOLID ", "BREATH", "WAVE  ", "BLINK ", "SCAN  ",
  };
  if (enabled == 0U) {
    return "OFF   ";
  }
  return (fx < CAPS_SIDE_FX_COUNT) ? k_names[fx] : "SOLID ";
}

/* Draw one compact key-value row (height 19 px, fits 4 rows cleanly on SYSTEM page) */
static void draw_value_row_compact(pixel_hud_t *hud, uint8_t row_idx, uint8_t y,
                                   char const *label, char const *val, uint16_t val_col)
{
  bool focused = (hud->focus_row == row_idx);
  bool editing = focused && hud->editing;
  uint16_t border = editing ? COL_AMBER : (focused ? COL_CYAN : COL_BORDER);
  uint16_t bg     = focused ? COL_CARD : COL_BG;

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 4U, y, 120U, 19U, border);
  batch_add(&b, 5U, (uint8_t)(y + 1U), 118U, 17U, bg);
  if (focused) {
    batch_add(&b, 5U, (uint8_t)(y + 1U), 3U, 17U, editing ? COL_AMBER : COL_CYAN);
  }
  batch_submit(hud, &b, false);

  q_text(hud, 12U, (uint8_t)(y + 4U), label, focused ? COL_WHITE : COL_MUTED, bg, 1U, false, false);
  q_text(hud, 62U, (uint8_t)(y + 4U), val, editing ? COL_AMBER : val_col, bg, 1U, false, false);
}

/* Draw one chunky key-value row (height 26 px) */
static void draw_value_row(pixel_hud_t *hud, uint8_t row_idx, uint8_t y,
                           char const *label, char const *val, uint16_t val_col)
{
  bool focused = (hud->focus_row == row_idx);
  bool editing = focused && hud->editing;
  uint16_t border = editing ? COL_AMBER : (focused ? COL_CYAN : COL_BORDER);
  uint16_t bg     = focused ? COL_CARD : COL_BG;

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 4U, y, 120U, 24U, border);
  batch_add(&b, 5U, (uint8_t)(y + 1U), 118U, 22U, bg);
  if (focused) {
    batch_add(&b, 5U, (uint8_t)(y + 1U), 3U, 22U, editing ? COL_AMBER : COL_CYAN);
  }
  batch_submit(hud, &b, false);

  q_text(hud, 12U, (uint8_t)(y + 6U), label, focused ? COL_WHITE : COL_MUTED, bg, 1U, false, false);
  q_text(hud, 62U, (uint8_t)(y + 6U), val, editing ? COL_AMBER : val_col, bg, 1U, false, false);
}

static char const *macro_speed_name(uint8_t idx)
{
  static char const *const k_names[5] = { "FAST", "NORM", "MED ", "SLOW", "SAFE" };
  uint8_t n = macro_player_normalize_speed(idx);
  return k_names[n - 1U];
}

static char const *macro_speed_detail(uint8_t idx)
{
  static char const *const k_det[5] = { "2ms .25x", "10ms 1x ", "25ms 2x ", "50ms 4x ", "100m 8x " };
  uint8_t n = macro_player_normalize_speed(idx);
  return k_det[n - 1U];
}

static void format_macro_slots(uint8_t const *macros, char out[8], bool *any_used)
{
  static char const k_keys[5] = { 'C', 'V', 'B', 'N', 'M' };
  bool any = false;
  for (uint8_t i = 0U; i < 5U; i++) {
    if (macro_recorder_slot_has_data(macros, i)) {
      out[i] = k_keys[i];
      any    = true;
    } else {
      out[i] = '-';
    }
  }
  out[5] = '\0';
  if (!any) {
    memcpy(out, "EMPTY", 6U);
  }
  *any_used = any;
}

static char const *rgb_fx_short_name(uint8_t fx)
{
  static char const *const k_names[7] = {
    "OFF   ", "WAVE  ", "SOLID ", "BREATH", "HE-MAP", "HEAT  ", "LAYER ",
  };
  return (fx < 7U) ? k_names[fx] : "CUSTOM";
}

static char const *side_fx_short_name(uint8_t enabled, uint8_t fx)
{
  static char const *const k_names[4] = {
    "OFF   ", "WAVE  ", "SOLID ", "BREATH",
  };
  if (enabled == 0U) {
    return "OFF   ";
  }
  return (fx < 4U) ? k_names[fx] : "WAVE  ";
}

typedef struct {
  char const *name;
  uint8_t     hue;
  uint8_t     sat;
} color_preset_t;

#define COLOR_PRESET_COUNT 9U

static color_preset_t const k_color_presets[COLOR_PRESET_COUNT] = {
  { "RED ", 0U,   255U },
  { "ORN ", 22U,  255U },
  { "YLW ", 42U,  255U },
  { "GRN ", 85U,  255U },
  { "CYN ", 128U, 255U },
  { "BLU ", 170U, 255U },
  { "PRP ", 198U, 255U },
  { "PNK ", 228U, 255U },
  { "WHT ", 0U,   0U   },
};

static uint8_t find_nearest_color_preset(uint8_t hue, uint8_t sat)
{
  if (sat < 48U) {
    return COLOR_PRESET_COUNT - 1U; /* WHT */
  }
  uint8_t best_idx  = 0U;
  uint8_t best_dist = 255U;
  for (uint8_t i = 0U; i < COLOR_PRESET_COUNT - 1U; i++) {
    uint8_t d = (uint8_t)(hue - k_color_presets[i].hue);
    if (d > 128U) {
      d = (uint8_t)(256U - d);
    }
    if (d < best_dist) {
      best_dist = d;
      best_idx  = i;
    }
  }
  return best_idx;
}

static uint16_t hsv_to_rgb565(uint8_t hue, uint8_t sat)
{
  rgb_grb_t c = rgb_hsv_to_grb(hue, sat, 255U);
  return (uint16_t)((((uint16_t)(c.r & 0xF8U)) << 8U) |
                    (((uint16_t)(c.g & 0xFCU)) << 3U) |
                    ((uint16_t)(c.b >> 3U)));
}

/* Draw compact color picker row with live RGB565 swatch box on the right */
static void draw_color_row_compact(pixel_hud_t *hud, uint8_t row_idx, uint8_t y,
                                   char const *label, uint8_t hue, uint8_t sat)
{
  bool focused = (hud->focus_row == row_idx);
  bool editing = focused && hud->editing;
  uint16_t border = editing ? COL_AMBER : (focused ? COL_CYAN : COL_BORDER);
  uint16_t bg     = focused ? COL_CARD : COL_BG;
  uint16_t swatch = hsv_to_rgb565(hue, sat);
  uint8_t  pidx   = find_nearest_color_preset(hue, sat);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 4U, y, 120U, 19U, border);
  batch_add(&b, 5U, (uint8_t)(y + 1U), 118U, 17U, bg);
  if (focused) {
    batch_add(&b, 5U, (uint8_t)(y + 1U), 3U, 17U, editing ? COL_AMBER : COL_CYAN);
  }
  /* Crisp 16x11 pixel live color swatch @ (102, y+4) */
  batch_add(&b, 101U, (uint8_t)(y + 3U), 18U, 13U, COL_WHITE);
  batch_add(&b, 102U, (uint8_t)(y + 4U), 16U, 11U, swatch);
  batch_submit(hud, &b, false);

  q_text(hud, 12U, (uint8_t)(y + 4U), label, focused ? COL_WHITE : COL_MUTED, bg, 1U, false, false);
  q_text(hud, 62U, (uint8_t)(y + 4U), k_color_presets[pidx].name,
         editing ? COL_AMBER : COL_WHITE, bg, 1U, false, false);
}

static void render_full_page(pixel_hud_t *hud)
{
  tk75_persist_cfg_t const *c = hud->cfg;

  if (hud->page == PIXEL_PAGE_HOME) {
    q_fill(hud, 0U, 19U, 128U, 92U, COL_BG, false);
    /* Left switch card frame */
    rect_batch_t b;
    batch_init(&b);
    batch_add(&b, 4U, 23U, 54U, 84U, COL_BORDER);
    batch_add(&b, 5U, 24U, 52U, 82U, COL_CARD);
    batch_submit(hud, &b, false);

    draw_home_header(hud);
    draw_switch_animation(hud, hud->anim_plunger_px, hud->live.any_actuated, false);
    draw_home_right_stats(hud, true, false);
    draw_footer(hud, true);
    return;
  }

  q_fill(hud, 0U, 19U, 128U, 92U, COL_BG, false);

  if (hud->page == PIXEL_PAGE_BRIGHTNESS) {
    draw_menu_header(hud, "BRIGHTNESS", 0U);
    draw_slider_row_compact(hud, 1U, 22U, "TFT", c->tft_brightness, COL_CYAN);
    draw_slider_row_compact(hud, 2U, 43U, "RGB", u8_to_pct10(c->rgb_brightness, c->rgb_enabled), COL_MINT);
    draw_slider_row_compact(hud, 3U, 64U, "SID", u8_to_pct10(c->side_brightness, c->side_enabled), COL_AMBER);
    draw_value_row_compact(hud, 4U, 85U, "CAP ",
                           caps_side_fx_short_name(c->caps_side_enabled, c->caps_side_effect),
                           (c->caps_side_enabled != 0U) ? COL_MINT : COL_MUTED);
    draw_footer(hud, true);
    return;
  }

  if (hud->page == PIXEL_PAGE_LIGHTING) {
    draw_menu_header(hud, "LIGHTING", 4U);
    draw_value_row_compact(hud, 1U, 22U, "FX  ",
                           rgb_fx_short_name((c->rgb_enabled != 0U) ? c->rgb_effect : 0U),
                           (c->rgb_enabled != 0U && c->rgb_effect != 0U) ? COL_CYAN : COL_MUTED);
    draw_color_row_compact(hud, 2U, 43U, "COL ", c->rgb_hue, c->rgb_sat);
    draw_value_row_compact(hud, 3U, 64U, "SFX ",
                           side_fx_short_name(c->side_enabled, c->side_effect),
                           (c->side_enabled != 0U && c->side_effect != 0U) ? COL_AMBER : COL_MUTED);
    draw_color_row_compact(hud, 4U, 85U, "SCOL", c->side_hue, c->side_sat);
    draw_footer(hud, true);
    return;
  }

  if (hud->page == PIXEL_PAGE_SWITCH) {
    draw_menu_header(hud, "HALL SWITCH", 1U);
    char act_str[6];
    fmt_mm_tenths((uint16_t)((c->hall_actuation_um + 50U) / 100U), act_str);
    draw_value_row_compact(hud, 1U, 22U, "ACT ", act_str, COL_CYAN);

    char rt_str[8] = "OFF";
    if (c->hall_rt_enabled != 0U) {
      fmt_mm_tenths((uint16_t)((c->hall_rt_down_um + 50U) / 100U), rt_str);
    }
    draw_value_row_compact(hud, 2U, 43U, "RT  ", rt_str, c->hall_rt_enabled ? COL_AMBER : COL_MUTED);

    char const *socd_str = (c->hall_socd_mode == HALL_SOCD_LAST_PRIORITY) ? "LAST"
                         : (c->hall_socd_mode == HALL_SOCD_NEUTRAL)       ? "NEUT"
                                                                          : "OFF";
    draw_value_row_compact(hud, 3U, 64U, "SOCD", socd_str,
                           (c->hall_socd_mode != HALL_SOCD_OFF) ? COL_MINT : COL_MUTED);
    draw_value_row_compact(hud, 4U, 85U, "CAL ", "START", COL_AMBER);
    draw_footer(hud, true);
    return;
  }

  if (hud->page == PIXEL_PAGE_MACRO) {
    draw_menu_header(hud, "MACRO", 2U);
    draw_value_row(hud, 1U, 24U, "SPD ", macro_speed_name(c->macro_speed_idx), COL_CYAN);
    draw_value_row(hud, 2U, 53U, "STEP", macro_speed_detail(c->macro_speed_idx), COL_AMBER);
    char slots_str[9];
    bool any_used = false;
    if (hud->focus_row == 3U && hud->editing) {
      static char const k_keys[5] = { 'C', 'V', 'B', 'N', 'M' };
      uint8_t s = hud->macro_sel_slot % 5U;
      bool    used = macro_recorder_slot_has_data(c->macros, s);
      slots_str[0] = k_keys[s];
      slots_str[1] = ':';
      memcpy(&slots_str[2], used ? "PLAY" : "----", 5U);
      draw_value_row(hud, 3U, 82U, "KEYS", slots_str, used ? COL_MINT : COL_MUTED);
    } else {
      format_macro_slots(c->macros, slots_str, &any_used);
      draw_value_row(hud, 3U, 82U, "KEYS", slots_str, any_used ? COL_MINT : COL_MUTED);
    }
    draw_footer(hud, true);
    return;
  }

  /* PIXEL_PAGE_SYSTEM (4 compact rows fitting y=22..103 cleanly) */
  draw_menu_header(hud, "SYSTEM", 3U);
  draw_slider_row_compact(hud, 1U, 22U, "SPD", u8_to_pct10(c->rgb_speed, 1U), COL_CYAN);
  draw_value_row_compact(hud, 2U, 43U, "OS  ", c->mac_mode ? "MACOS" : "WIN/LNX", COL_MINT);
  draw_value_row_compact(hud, 3U, 64U, "SAVR", c->tft_screensaver ? "ON " : "OFF",
                         c->tft_screensaver ? COL_MINT : COL_MUTED);
  draw_value_row_compact(hud, 4U, 85U, "UI  ", hud->stock_ui_mode ? "STOCK" : "PIXEL", COL_AMBER);
  draw_footer(hud, true);
}

/* Draw static chrome of fullscreen pixel-art macro recording screen (128x128) */
static void draw_macro_rec_static(pixel_hud_t *hud)
{
  q_fill(hud, 0U, 0U, 128U, 128U, COL_BG, false);

  rect_batch_t b;
  batch_init(&b);
  /* Outer viewfinder corner brackets */
  batch_add(&b, 2U, 2U, 12U, 2U, COL_CORAL);
  batch_add(&b, 2U, 2U, 2U, 12U, COL_CORAL);
  batch_add(&b, 114U, 2U, 12U, 2U, COL_CORAL);
  batch_add(&b, 124U, 2U, 2U, 12U, COL_CORAL);
  batch_add(&b, 2U, 124U, 12U, 2U, COL_CORAL);
  batch_add(&b, 2U, 114U, 2U, 12U, COL_CORAL);
  batch_add(&b, 114U, 124U, 12U, 2U, COL_CORAL);
  batch_add(&b, 124U, 114U, 2U, 12U, COL_CORAL);

  /* Retro cassette tape deck body (x=12..115, y=28..74) */
  batch_add(&b, 12U, 28U, 104U, 46U, COL_BORDER);
  batch_add(&b, 14U, 30U, 100U, 42U, COL_CARD);
  batch_add(&b, 18U, 34U, 92U, 26U, COL_DIM);     /* Tape reel window */
  batch_add(&b, 28U, 64U, 72U, 8U, COL_HOUSING);  /* Cassette bottom trapezoid */
  batch_add(&b, 36U, 66U, 6U, 4U, COL_BG);        /* Capstan hole L */
  batch_add(&b, 86U, 66U, 6U, 4U, COL_BG);        /* Capstan hole R */
  batch_submit(hud, &b, false);

  /* Header title: "REC M0 (C)" (or "REC M5" for slots >= 5) */
  static char const k_slot_keys[5] = { 'C', 'V', 'B', 'N', 'M' };
  char hdr[12];
  uint8_t s = hud->live.macro_rec_slot;
  hdr[0] = 'R'; hdr[1] = 'E'; hdr[2] = 'C'; hdr[3] = ' '; hdr[4] = 'M';
  if (s < 10U) {
    hdr[5] = (char)('0' + s);
    if (s < 5U) {
      hdr[6] = ' '; hdr[7] = '('; hdr[8] = k_slot_keys[s]; hdr[9] = ')'; hdr[10] = '\0';
    } else {
      hdr[6] = '\0';
    }
  } else {
    hdr[5] = '1'; hdr[6] = (char)('0' + (s - 10U)); hdr[7] = '\0';
  }
  q_text(hud, 24U, 8U, hdr, COL_WHITE, COL_BG, 1U, false, false);
  q_text(hud, 14U, 80U, "EVTS:", COL_MUTED, COL_BG, 1U, false, false);
  q_text(hud, 66U, 80U, "/341", COL_MUTED, COL_BG, 1U, false, false);
  q_text(hud, 14U, 109U, "FN:EDIT / SAVE", COL_AMBER, COL_BG, 1U, false, false);
}

/* Draw 1 animated frame of fullscreen macro recorder (blinking REC dot + spinning reels + meter) */
static void draw_macro_rec_anim(pixel_hud_t *hud, bool force_text)
{
  uint8_t  phase   = hud->rec_anim_phase & 3U;
  bool     dot_on  = (phase < 2U);
  uint16_t bytes   = hud->live.macro_rec_bytes;
  if (bytes > MACRO_REC_MAX_BYTES) {
    bytes = MACRO_REC_MAX_BYTES;
  }
  uint16_t evts = (uint16_t)(bytes / 3U);

  /* Batch 1: Blinking REC dot + dual spinning pixel tape reels */
  rect_batch_t b;
  batch_init(&b);
  uint16_t dot_col = dot_on ? COL_CORAL : COL_DIM;
  batch_add(&b, 12U, 10U, 8U, 8U, dot_col);
  if (dot_on) {
    batch_add(&b, 14U, 12U, 2U, 2U, COL_WHITE); /* Pixel shine on REC dot */
  }

  /* Clear reel interiors + tape bridge */
  batch_add(&b, 26U, 38U, 18U, 18U, COL_DIM);
  batch_add(&b, 84U, 38U, 18U, 18U, COL_DIM);
  batch_add(&b, 48U, 42U, 32U, 10U, COL_BG);

  /* Left & right reel rims + center hubs */
  batch_add(&b, 28U, 40U, 14U, 14U, COL_BORDER);
  batch_add(&b, 30U, 42U, 10U, 10U, COL_BG);
  batch_add(&b, 86U, 40U, 14U, 14U, COL_BORDER);
  batch_add(&b, 88U, 42U, 10U, 10U, COL_BG);
  batch_submit(hud, &b, false);

  /* Batch 2: Rotating pixel-art reel spokes + hub centers + animated tape pulse (<=12 rects) */
  batch_init(&b);
  uint16_t spk = COL_CYAN;
  if ((phase & 1U) == 0U) {
    /* '+' cross orientation */
    batch_add(&b, 34U, 41U, 2U, 12U, spk);
    batch_add(&b, 29U, 46U, 12U, 2U, spk);
    batch_add(&b, 92U, 41U, 2U, 12U, spk);
    batch_add(&b, 87U, 46U, 12U, 2U, spk);
  } else {
    /* 'x' diagonal orientation */
    batch_add(&b, 31U, 43U, 3U, 3U, spk);
    batch_add(&b, 36U, 48U, 3U, 3U, spk);
    batch_add(&b, 36U, 43U, 3U, 3U, spk);
    batch_add(&b, 31U, 48U, 3U, 3U, spk);
    batch_add(&b, 89U, 43U, 3U, 3U, spk);
    batch_add(&b, 94U, 48U, 3U, 3U, spk);
    batch_add(&b, 94U, 43U, 3U, 3U, spk);
    batch_add(&b, 89U, 48U, 3U, 3U, spk);
  }
  batch_add(&b, 33U, 45U, 4U, 4U, COL_WHITE);
  batch_add(&b, 91U, 45U, 4U, 4U, COL_WHITE);

  /* Animated moving magnetic tape data bars in center window */
  uint8_t wave_x = (uint8_t)(50U + (phase * 6U));
  batch_add(&b, wave_x, 45U, 4U, 4U, COL_CORAL);
  uint8_t wave_x2 = (uint8_t)(50U + (((phase + 2U) & 3U) * 6U));
  batch_add(&b, wave_x2, 46U, 4U, 2U, COL_AMBER);
  batch_submit(hud, &b, false);

  /* Batch 3: Live buffer fill bar (x=14..113, y=95, w=100, h=8) */
  batch_init(&b);
  batch_add(&b, 13U, 94U, 102U, 10U, COL_BORDER);
  batch_add(&b, 14U, 95U, 100U, 8U, COL_DIM);
  uint8_t fill_w = (uint8_t)(((uint32_t)bytes * 100U) / MACRO_REC_MAX_BYTES);
  if (fill_w > 100U) fill_w = 100U;
  if (fill_w > 0U) {
    uint16_t bar_col = (fill_w >= 80U) ? COL_CORAL : ((fill_w >= 50U) ? COL_AMBER : COL_MINT);
    batch_add(&b, 14U, 95U, fill_w, 8U, bar_col);
  }
  bool text_chg = force_text || (bytes != hud->prev_rec_bytes);
  batch_submit(hud, &b, !text_chg);

  if (text_chg) {
    char cbuf[4];
    fmt_wpm3(evts, cbuf);
    q_text(hud, 46U, 80U, cbuf, COL_WHITE, COL_BG, 1U, false, true);
    hud->prev_rec_bytes = bytes;
  }
}

/* ---- Fullscreen 128x128 Pixel-Art Macro Text Editor (5 rows x 19 cols, German ISO) ---- */

#define EDIT_VIEW_ROWS 5U
#define EDIT_VIEW_COLS MACRO_EDITOR_COLS

/* Map one editable macro token to a 1-char German ISO display glyph + semantic color */
static void macro_tok_to_cell(macro_edit_tok_t tok, char *out_ch, uint16_t *out_fg)
{
  bool shift  = ((tok.mods & 0x22U) != 0U);
  bool altgr  = ((tok.mods & 0x40U) != 0U);
  bool combo  = ((tok.mods & 0x9DU) != 0U); /* Ctrl, LAlt, LGui, RGui */
  uint8_t hid = tok.hid;
  char ch     = '?';
  uint16_t fg = combo ? COL_AMBER : COL_WHITE;

  if (hid >= (uint8_t)KC_A && hid <= (uint8_t)KC_Z) {
    char base = (char)('a' + (hid - (uint8_t)KC_A));
    /* German QWERTZ swap: HID Y <-> 'z', HID Z <-> 'y' */
    if (hid == (uint8_t)KC_Y) base = 'z';
    else if (hid == (uint8_t)KC_Z) base = 'y';
    if (altgr && !combo && hid == (uint8_t)KC_Q) {
      ch = '@';
      fg = COL_MINT;
    } else if (altgr && !combo && hid == (uint8_t)KC_E) {
      ch = 'E'; /* Euro sign indicator */
      fg = COL_MINT;
    } else {
      ch = (shift && !combo) ? (char)(base - 'a' + 'A') : base;
    }
  } else if (hid >= (uint8_t)KC_1 && hid <= (uint8_t)KC_0) {
    static char const k_norm[10] = "1234567890";
    static char const k_shft[10] = "!\"$%&/()=";
    uint8_t idx = (uint8_t)(hid - (uint8_t)KC_1);
    if (altgr && !combo) {
      if (hid == (uint8_t)KC_7)      { ch = '{'; fg = COL_MINT; }
      else if (hid == (uint8_t)KC_8) { ch = '['; fg = COL_MINT; }
      else if (hid == (uint8_t)KC_9) { ch = ']'; fg = COL_MINT; }
      else if (hid == (uint8_t)KC_0) { ch = '}'; fg = COL_MINT; }
      else                           { ch = k_norm[idx]; }
    } else if (shift && !combo) {
      ch = (idx == 9U) ? '=' : k_shft[idx];
      if (idx == 2U) {
        fg = COL_MINT; /* § on DE Shift+3 */
      }
    } else {
      ch = k_norm[idx];
    }
  } else {
    switch (hid) {
      case KC_SPC:  ch = ' '; break;
      case KC_ENT:  ch = '>'; fg = combo ? COL_AMBER : COL_CYAN; break;
      case KC_TAB:  ch = 'T'; fg = combo ? COL_AMBER : COL_CYAN; break;
      case KC_BSPC: ch = '<'; fg = combo ? COL_AMBER : COL_CORAL; break;
      case KC_DEL:  ch = 'X'; fg = combo ? COL_AMBER : COL_CORAL; break;
      case KC_ESC:  ch = '!'; fg = COL_CORAL; break;
      case KC_MINS: ch = altgr ? '\\' : (shift ? '?' : 's'); if (!shift && !altgr && !combo) fg = COL_MINT; break;
      case KC_EQL:  ch = shift ? '`' : '\''; break;
      case KC_LBRC: ch = shift ? 'U' : 'u'; if (!combo) fg = COL_MINT; break; /* ü/Ü */
      case KC_RBRC: ch = altgr ? '~' : (shift ? '*' : '+'); break;
      case KC_SCLN: ch = shift ? 'O' : 'o'; if (!combo) fg = COL_MINT; break; /* ö/Ö */
      case KC_QUOT: ch = shift ? 'A' : 'a'; if (!combo) fg = COL_MINT; break; /* ä/Ä */
      case KC_GRV:  ch = '^'; break;
      case KC_NUHS: ch = shift ? '\'' : '#'; break;
      case KC_NUBS: ch = altgr ? '|' : (shift ? '>' : '<'); break;
      case KC_COMM: ch = shift ? ';' : ','; break;
      case KC_DOT:  ch = shift ? ':' : '.'; break;
      case KC_SLSH: ch = shift ? '_' : '-'; break;
      case KC_LEFT: ch = '<'; fg = COL_CYAN; break;
      case KC_RGHT: ch = '>'; fg = COL_CYAN; break;
      case KC_UP:   ch = '^'; fg = COL_CYAN; break;
      case KC_DOWN: ch = 'v'; fg = COL_CYAN; break;
      default:
        ch = (hid >= (uint8_t)KC_F1 && hid <= (uint8_t)KC_F12) ? 'F'
           : (hid >= 0xE0U && hid <= 0xE7U)                    ? 'M'
                                                               : '*';
        fg = combo ? COL_AMBER : COL_CYAN;
        break;
    }
  }

  *out_ch = ch;
  *out_fg = fg;
}

static uint8_t append_str(char *dst, uint8_t pos, uint8_t max_len, char const *s)
{
  while (*s != '\0' && pos < max_len) {
    dst[pos++] = *s++;
  }
  return pos;
}

/* Format fixed 20-char human-readable inspector line for token under cursor */
static void fmt_macro_tok_inspector(pixel_hud_live_t const *l, char out[21], uint16_t *out_col)
{
  memset(out, ' ', 20U);
  out[20] = '\0';

  uint16_t cur = l->macro_edit_cursor;
  if (cur > l->macro_edit_count) {
    cur = l->macro_edit_count;
  }
  uint8_t p = 0U;
  out[p++] = '#';
  out[p++] = (char)('0' + ((cur / 100U) % 10U));
  out[p++] = (char)('0' + ((cur / 10U) % 10U));
  out[p++] = (char)('0' + (cur % 10U));
  out[p++] = ' ';

  if (l->macro_edit_toks == NULL || cur >= l->macro_edit_count) {
    (void)append_str(out, p, 20U, "[END] TYPE...");
    *out_col = COL_CYAN;
    return;
  }

  macro_edit_tok_t tok = l->macro_edit_toks[cur];
  bool has_mod_prefix = false;
  if ((tok.mods & 0x11U) != 0U) { p = append_str(out, p, 20U, "CTL+"); has_mod_prefix = true; }
  if ((tok.mods & 0x04U) != 0U) { p = append_str(out, p, 20U, "ALT+"); has_mod_prefix = true; }
  if ((tok.mods & 0x88U) != 0U) { p = append_str(out, p, 20U, "WIN+"); has_mod_prefix = true; }
  if (has_mod_prefix && (tok.mods & 0x22U) != 0U) { p = append_str(out, p, 20U, "SHF+"); }

  char ch = '?';
  uint16_t fg = COL_WHITE;
  macro_tok_to_cell(tok, &ch, &fg);
  *out_col = has_mod_prefix ? COL_AMBER : fg;

  switch (tok.hid) {
    case KC_SPC:  (void)append_str(out, p, 20U, "SPACE"); break;
    case KC_ENT:  (void)append_str(out, p, 20U, "ENTER"); break;
    case KC_TAB:  (void)append_str(out, p, 20U, "TAB"); break;
    case KC_BSPC: (void)append_str(out, p, 20U, "BKSP"); break;
    case KC_DEL:  (void)append_str(out, p, 20U, "DEL"); break;
    case KC_ESC:  (void)append_str(out, p, 20U, "ESC"); break;
    case KC_LEFT: (void)append_str(out, p, 20U, "LEFT"); break;
    case KC_RGHT: (void)append_str(out, p, 20U, "RIGHT"); break;
    case KC_UP:   (void)append_str(out, p, 20U, "UP"); break;
    case KC_DOWN: (void)append_str(out, p, 20U, "DOWN"); break;
    case KC_HOME: (void)append_str(out, p, 20U, "HOME"); break;
    case KC_END:  (void)append_str(out, p, 20U, "END"); break;
    case KC_LBRC: (void)append_str(out, p, 20U, ((tok.mods & 0x22U) != 0U) ? "'UE' (DE)" : "'ue' (DE)"); break;
    case KC_SCLN: (void)append_str(out, p, 20U, ((tok.mods & 0x22U) != 0U) ? "'OE' (DE)" : "'oe' (DE)"); break;
    case KC_QUOT: (void)append_str(out, p, 20U, ((tok.mods & 0x22U) != 0U) ? "'AE' (DE)" : "'ae' (DE)"); break;
    case KC_MINS:
      if ((tok.mods & 0x62U) == 0U) { (void)append_str(out, p, 20U, "'ss' (DE)"); break; }
      /* fallthrough */
    default:
      if (tok.hid >= (uint8_t)KC_F1 && tok.hid <= (uint8_t)KC_F12) {
        uint8_t fn = (uint8_t)(tok.hid - (uint8_t)KC_F1 + 1U);
        if (p < 20U) out[p++] = 'F';
        if (fn >= 10U && p < 20U) out[p++] = (char)('0' + (fn / 10U));
        if (p < 20U) out[p++] = (char)('0' + (fn % 10U));
      } else if (tok.hid >= 0xE0U && tok.hid <= 0xE7U) {
        static char const *const k_mods[8] = {
          "LCTL", "LSHF", "LALT", "LWIN", "RCTL", "RSHF", "ALTGR", "RWIN"
        };
        (void)append_str(out, p, 20U, k_mods[tok.hid - 0xE0U]);
      } else {
        if (p + 3U <= 20U) {
          out[p++] = '\'';
          out[p++] = ch;
          out[p++] = '\'';
        }
      }
      break;
  }
}

static void draw_macro_edit_static(pixel_hud_t *hud)
{
  q_fill(hud, 0U, 0U, 128U, 128U, COL_BG, false);

  rect_batch_t b;
  batch_init(&b);
  /* Header (y=0..18) + editor frame (x=2..125, y=20..93) + inspector (y=95..109) + footer (y=111..127) */
  batch_add(&b, 0U, 0U, 128U, 18U, COL_CARD);
  batch_add(&b, 0U, 18U, 128U, 1U, COL_CYAN);
  batch_add(&b, 2U, 20U, 124U, 74U, COL_BORDER);
  batch_add(&b, 3U, 21U, 122U, 72U, COL_BG);
  batch_add(&b, 0U, 95U, 128U, 15U, COL_CARD);
  batch_add(&b, 0U, 110U, 128U, 1U, COL_BORDER);
  batch_add(&b, 0U, 111U, 128U, 17U, COL_CARD);
  batch_submit(hud, &b, false);

  char hdr[10] = "EDIT M0  ";
  uint8_t s = hud->live.macro_rec_slot;
  if (s < 10U) {
    hdr[6] = (char)('0' + s);
    hdr[7] = '\0';
  } else {
    hdr[6] = '1';
    hdr[7] = (char)('0' + (s - 10U));
    hdr[8] = '\0';
  }
  q_text(hud, 4U, 3U, hdr, COL_CYAN, COL_CARD, 1U, false, false);
  q_text(hud, 4U, 113U, "FN:SAVE", COL_MINT, COL_CARD, 1U, false, false);
  q_text(hud, 68U, 113U, "KNOB:QUIT", COL_CORAL, COL_CARD, 1U, false, false);
}

/* Render one 19-char editor line using color run-length grouping (fast & queue-safe) */
static void draw_macro_edit_row(pixel_hud_t *hud, uint8_t vis_row, bool flush)
{
  pixel_hud_live_t const *l = &hud->live;
  uint16_t base = (uint16_t)((uint16_t)(hud->edit_scroll_row + vis_row) * EDIT_VIEW_COLS);
  uint8_t  y    = (uint8_t)(22U + (vis_row * 14U));

  char     chars[EDIT_VIEW_COLS];
  uint16_t fgs[EDIT_VIEW_COLS];
  uint16_t bgs[EDIT_VIEW_COLS];

  for (uint8_t col = 0U; col < EDIT_VIEW_COLS; col++) {
    uint16_t idx = (uint16_t)(base + col);
    bool is_cur  = (idx == l->macro_edit_cursor);
    char ch      = ' ';
    uint16_t fg  = COL_WHITE;
    uint16_t bg  = COL_BG;

    if (l->macro_edit_toks != NULL && idx < l->macro_edit_count) {
      macro_tok_to_cell(l->macro_edit_toks[idx], &ch, &fg);
    }
    if (is_cur) {
      bg = hud->edit_blink_on ? COL_CYAN : COL_AMBER;
      fg = COL_BG;
    }
    chars[col] = ch;
    fgs[col]   = fg;
    bgs[col]   = bg;
  }

  uint8_t col = 0U;
  while (col < EDIT_VIEW_COLS) {
    uint8_t  start = col;
    uint16_t fg    = fgs[start];
    uint16_t bg    = bgs[start];
    char     run[EDIT_VIEW_COLS + 1U];
    uint8_t  rlen  = 0U;
    while (col < EDIT_VIEW_COLS && fgs[col] == fg && bgs[col] == bg) {
      run[rlen++] = chars[col++];
    }
    run[rlen] = '\0';
    bool is_last = (col >= EDIT_VIEW_COLS);
    q_text(hud, (uint8_t)(6U + (start * 6U)), y, run, fg, bg, 1U, false, flush && is_last);
  }
}

static void draw_macro_edit_meta(pixel_hud_t *hud, bool flush)
{
  pixel_hud_live_t const *l = &hud->live;
  uint16_t cnt = l->macro_edit_count;
  if (cnt > MACRO_EDITOR_MAX_TOKS) cnt = MACRO_EDITOR_MAX_TOKS;

  char cbuf[8];
  cbuf[0] = (cnt >= 100U) ? (char)('0' + ((cnt / 100U) % 10U)) : ' ';
  cbuf[1] = (cnt >= 10U)  ? (char)('0' + ((cnt / 10U) % 10U))  : ' ';
  cbuf[2] = (char)('0' + (cnt % 10U));
  cbuf[3] = '/';
  cbuf[4] = '2';
  cbuf[5] = '5';
  cbuf[6] = '6';
  cbuf[7] = '\0';
  uint16_t ccol = (cnt >= 240U) ? COL_CORAL : ((cnt >= 192U) ? COL_AMBER : COL_WHITE);
  q_text(hud, 82U, 3U, cbuf, ccol, COL_CARD, 1U, false, false);

  char insp[21];
  uint16_t icol = COL_WHITE;
  fmt_macro_tok_inspector(l, insp, &icol);
  q_text(hud, 4U, 96U, insp, icol, COL_CARD, 1U, false, false);

  /* Vertical scrollbar @ (122, 22, w=2, h=70) */
  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 122U, 22U, 2U, 70U, COL_DIM);
  uint8_t total_rows = (uint8_t)((cnt / EDIT_VIEW_COLS) + 1U);
  if (total_rows > EDIT_VIEW_ROWS) {
    uint8_t thumb_h = (uint8_t)((70U * EDIT_VIEW_ROWS) / total_rows);
    if (thumb_h < 10U) thumb_h = 10U;
    uint8_t max_scroll = (uint8_t)(total_rows - EDIT_VIEW_ROWS);
    uint8_t thumb_y = (uint8_t)(22U + (((uint32_t)hud->edit_scroll_row * (70U - thumb_h)) / max_scroll));
    batch_add(&b, 122U, thumb_y, 2U, thumb_h, COL_CYAN);
  }
  batch_submit(hud, &b, flush);
}

static void step_macro_edit_hud(pixel_hud_t *hud, uint32_t now_ms)
{
  pixel_hud_live_t const *l = &hud->live;
  uint8_t cur_row = (uint8_t)(l->macro_edit_cursor / EDIT_VIEW_COLS);

  if (l->macro_edit_rev != hud->prev_edit_rev) {
    uint8_t old_scroll = hud->edit_scroll_row;
    if (cur_row < hud->edit_scroll_row) {
      hud->edit_scroll_row = cur_row;
    } else if (cur_row >= (uint8_t)(hud->edit_scroll_row + EDIT_VIEW_ROWS)) {
      hud->edit_scroll_row = (uint8_t)(cur_row - (EDIT_VIEW_ROWS - 1U));
    }
    hud->edit_blink_on = true;
    hud->last_anim_ms  = now_ms;

    if (hud->edit_scroll_row != old_scroll) {
      hud->edit_dirty_mask = 0x3FU;
    } else {
      uint8_t prev_row = (uint8_t)(hud->prev_edit_cursor / EDIT_VIEW_COLS);
      uint8_t min_r    = (cur_row < prev_row) ? cur_row : prev_row;
      if (min_r < hud->edit_scroll_row) min_r = hud->edit_scroll_row;
      for (uint8_t vr = 0U; vr < EDIT_VIEW_ROWS; vr++) {
        if ((uint8_t)(hud->edit_scroll_row + vr) >= min_r) {
          hud->edit_dirty_mask |= (uint8_t)(1U << vr);
        }
      }
      hud->edit_dirty_mask |= 0x20U;
    }
    hud->prev_edit_rev    = l->macro_edit_rev;
    hud->prev_edit_cursor = l->macro_edit_cursor;
  } else if (hud->edit_dirty_mask == 0U && (uint32_t)(now_ms - hud->last_anim_ms) >= 450U) {
    hud->last_anim_ms  = now_ms;
    hud->edit_blink_on = !hud->edit_blink_on;
    if (cur_row >= hud->edit_scroll_row && cur_row < (uint8_t)(hud->edit_scroll_row + EDIT_VIEW_ROWS)) {
      hud->edit_dirty_mask |= (uint8_t)(1U << (cur_row - hud->edit_scroll_row));
    }
  }

  /* Drain dirty rows + meta without ever overflowing the 40-op UART7 queue */
  while (hud->edit_dirty_mask != 0U && hud->q_count <= 18U) {
    uint8_t bit = 0U;
    while (bit < 6U && (hud->edit_dirty_mask & (uint8_t)(1U << bit)) == 0U) {
      bit++;
    }
    if (bit >= 6U) {
      hud->edit_dirty_mask = 0U;
      break;
    }
    hud->edit_dirty_mask &= (uint8_t)(~(1U << bit));
    bool last_piece = (hud->edit_dirty_mask == 0U);
    if (bit < EDIT_VIEW_ROWS) {
      draw_macro_edit_row(hud, bit, last_piece);
    } else {
      draw_macro_edit_meta(hud, last_piece);
    }
  }
}

static void fmt_u8_zero2(uint8_t v, char out[3])
{
  if (v > 99U) {
    v = 99U;
  }
  out[0] = (char)('0' + (v / 10U));
  out[1] = (char)('0' + (v % 10U));
  out[2] = '\0';
}

static void fmt_keys_compact(uint32_t keys, char out[8])
{
  out[0] = 'K';
  out[1] = ':';
  if (keys >= 10000U) {
    uint32_t k = keys / 1000U;
    if (k > 99U) k = 99U;
    out[2] = (char)('0' + (k / 10U));
    out[3] = (char)('0' + (k % 10U));
    out[4] = 'k';
    out[5] = ' ';
    out[6] = '\0';
    return;
  }
  if (keys >= 1000U) {
    out[2] = (char)('0' + ((keys / 1000U) % 10U));
    out[3] = (char)('0' + ((keys / 100U) % 10U));
    out[4] = (char)('0' + ((keys / 10U) % 10U));
    out[5] = (char)('0' + (keys % 10U));
    out[6] = '\0';
    return;
  }
  out[2] = (keys >= 100U) ? (char)('0' + ((keys / 100U) % 10U)) : ' ';
  out[3] = (keys >= 10U)  ? (char)('0' + ((keys / 10U) % 10U))  : ' ';
  out[4] = (char)('0' + (keys % 10U));
  out[5] = ' ';
  out[6] = '\0';
}

static uint32_t compute_saver_hash(pixel_hud_t const *hud)
{
  pixel_hud_live_t const   *l = &hud->live;
  tk75_persist_cfg_t const *c = hud->cfg;
  uint32_t h = 2166136261U;
  h = (h ^ l->battery_pct) * 16777619U;
  h = (h ^ l->conn_mode) * 16777619U;
  h = (h ^ l->bt_slot) * 16777619U;
  h = (h ^ l->active_layer) * 16777619U;
  h = (h ^ (l->charging ? 1U : 0U)) * 16777619U;
  h = (h ^ (l->vbus_present ? 1U : 0U)) * 16777619U;
  h = (h ^ (l->rtc_valid ? 1U : 0U)) * 16777619U;
  h = (h ^ l->host_leds) * 16777619U;
  h = (h ^ (c->mac_mode ? 1U : 0U)) * 16777619U;
  h = (h ^ (c->win_lock ? 1U : 0U)) * 16777619U;
  h = (h ^ l->peak_wpm) * 16777619U;
  h = (h ^ l->session_keys) * 16777619U;
  h = (h ^ (c->hall_rt_enabled ? 1U : 0U)) * 16777619U;
  h = (h ^ c->hall_socd_mode) * 16777619U;
  return h;
}

/* Draw static chrome of fullscreen pixel-art idle screensaver (128x128) */
static void draw_screensaver_static(pixel_hud_t *hud)
{
  q_fill(hud, 0U, 0U, 128U, 128U, COL_BG, false);

  rect_batch_t b;
  batch_init(&b);
  /* Top sky divider + center hero clock card (x=4..123, y=25..68) */
  batch_add(&b, 0U, 22U, 128U, 1U, COL_BORDER);
  batch_add(&b, 4U, 25U, 120U, 43U, COL_BORDER);
  batch_add(&b, 5U, 26U, 118U, 41U, COL_CARD);

  /* Two system telemetry cards (y=72..108) */
  batch_add(&b, 4U, 72U, 58U, 36U, COL_BORDER);
  batch_add(&b, 5U, 73U, 56U, 34U, COL_CARD);
  batch_add(&b, 66U, 72U, 58U, 36U, COL_BORDER);
  batch_add(&b, 67U, 73U, 56U, 34U, COL_CARD);

  /* Bottom status strip (y=111..127) */
  batch_add(&b, 0U, 111U, 128U, 1U, COL_BORDER);
  batch_add(&b, 0U, 112U, 128U, 16U, COL_CARD);

  /* Pixel-art crescent moon in top-left sky (x=6..15, y=4..14) */
  batch_add(&b, 8U, 4U, 5U, 2U, COL_AMBER);
  batch_add(&b, 6U, 6U, 4U, 6U, COL_AMBER);
  batch_add(&b, 8U, 12U, 5U, 2U, COL_AMBER);
  batch_add(&b, 10U, 6U, 3U, 6U, COL_BG); /* Crescent cutout */
  batch_submit(hud, &b, false);
}

/* Draw live screensaver telemetry cards + top battery + bottom lock/keystroke strip */
static void draw_screensaver_sysinfo(pixel_hud_t *hud, bool flush)
{
  pixel_hud_live_t const   *l = &hud->live;
  tk75_persist_cfg_t const *c = hud->cfg;

  /* Top-right power indicator (USB charging bolt / USB full plug / battery) + battery icon */
  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 63U, 3U, 30U, 15U, COL_BG); /* Clear previous bolt/plug/battery cleanly */
  uint16_t batt_col = batch_add_battery_widget(&b, 5U, l);
  batch_submit(hud, &b, false);

  q_text(hud, 20U, 5U, l->rtc_valid ? "CLK" : "UP ", l->rtc_valid ? COL_CYAN : COL_AMBER,
         COL_BG, 1U, false, false);
  char bstr[5];
  fmt_u8_pct(l->battery_pct, bstr);
  q_text(hud, 95U, 4U, bstr, batt_col, COL_BG, 1U, false, false);

  /* Left card: Transport/poll rate + OS/Layer */
  char tr_str[8] = "USB 8K";
  uint16_t tr_col = COL_CYAN;
  if (l->conn_mode == 1U) {
    memcpy(tr_str, "2.4 1K", 7U);
    tr_col = COL_MINT;
  } else if (l->conn_mode == 2U) {
    tr_str[0] = 'B'; tr_str[1] = 'T'; tr_str[2] = (char)('1' + (l->bt_slot % 3U));
    tr_str[3] = ' '; tr_str[4] = 'L'; tr_str[5] = 'E'; tr_str[6] = '\0';
    tr_col = COL_AMBER;
  }
  q_text(hud, 9U, 76U, tr_str, tr_col, COL_CARD, 1U, false, false);

  char os_str[7];
  if (c->mac_mode) {
    os_str[0] = 'M'; os_str[1] = 'A'; os_str[2] = 'C';
  } else {
    os_str[0] = 'W'; os_str[1] = 'I'; os_str[2] = 'N';
  }
  os_str[3] = ' ';
  os_str[4] = 'L';
  os_str[5] = (char)('0' + (l->active_layer & 3U));
  os_str[6] = '\0';
  q_text(hud, 9U, 92U, os_str, COL_WHITE, COL_CARD, 1U, false, false);

  /* Right card: Hall actuation + RT / Peak WPM */
  char act_str[8];
  uint16_t tenths = (uint16_t)((c->hall_actuation_um + 50U) / 100U);
  if (tenths > 99U) tenths = 99U;
  act_str[0] = (char)('0' + (tenths / 10U));
  act_str[1] = '.';
  act_str[2] = (char)('0' + (tenths % 10U));
  if (c->hall_rt_enabled) {
    act_str[3] = ' '; act_str[4] = 'R'; act_str[5] = 'T'; act_str[6] = '\0';
  } else {
    act_str[3] = 'm'; act_str[4] = 'm'; act_str[5] = ' '; act_str[6] = '\0';
  }
  q_text(hud, 71U, 76U, act_str, c->hall_rt_enabled ? COL_MINT : COL_CYAN, COL_CARD, 1U, false, false);

  char pk_str[8] = "PK    ";
  char w3[4];
  fmt_wpm3(l->peak_wpm, w3);
  pk_str[3] = w3[0];
  pk_str[4] = w3[1];
  pk_str[5] = w3[2];
  pk_str[6] = '\0';
  q_text(hud, 71U, 92U, pk_str, COL_AMBER, COL_CARD, 1U, false, false);

  /* Bottom strip: Lock flags or SOCD on left, Session keystrokes on right */
  char left_b[10] = "TYPE:WAKE";
  uint16_t left_col = COL_MUTED;
  if ((l->host_leds & 0x02U) != 0U) {
    memcpy(left_b, "CAPS ON  ", 10U);
    left_col = COL_CORAL;
  } else if (c->win_lock != 0U) {
    memcpy(left_b, "WIN LOCK ", 10U);
    left_col = COL_AMBER;
  } else if ((l->host_leds & 0x01U) != 0U) {
    memcpy(left_b, "NUM LOCK ", 10U);
    left_col = COL_CYAN;
  } else if (c->hall_socd_mode != HALL_SOCD_OFF) {
    memcpy(left_b, (c->hall_socd_mode == HALL_SOCD_LAST_PRIORITY) ? "SOCD:LAST" : "SOCD:NEUT", 10U);
    left_col = COL_MINT;
  }
  q_text(hud, 5U, 114U, left_b, left_col, COL_CARD, 1U, false, false);

  char kbuf[8];
  fmt_keys_compact(l->session_keys, kbuf);
  q_text(hud, 82U, 114U, kbuf, COL_WHITE, COL_CARD, 1U, false, flush);

  hud->prev_saver_hash = compute_saver_hash(hud);
}

/* Draw animated screensaver tick: twinkling stars + big 2x pixel clock + seconds sweep bar */
static void draw_screensaver_anim(pixel_hud_t *hud, bool force_all)
{
  pixel_hud_live_t const   *l = &hud->live;
  tk75_persist_cfg_t const *c = hud->cfg;
  uint8_t phase = hud->saver_phase & 3U;

  uint8_t disp_h = 0U;
  uint8_t disp_m = 0U;
  uint8_t disp_s = 0U;
  if (l->rtc_valid) {
    disp_h = l->rtc_hour;
    if (c->tft_clock_24h == 0U) {
      disp_h = (uint8_t)(disp_h % 12U);
      if (disp_h == 0U) disp_h = 12U;
    }
    disp_m = l->rtc_min;
    disp_s = l->rtc_sec;
  } else {
    uint32_t up = l->uptime_sec;
    disp_h = (uint8_t)((up / 3600U) % 100U);
    disp_m = (uint8_t)((up / 60U) % 60U);
    disp_s = (uint8_t)(up % 60U);
  }

  /* Twinkling pixel-art stars in top sky (x=40..61, y=3..18, clear of USB/bolt icon @ x=64..73) */
  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 40U, 3U, 22U, 17U, COL_BG);
  batch_add(&b, 42U, 6U, 2U, 2U, (phase == 0U) ? COL_WHITE : COL_DIM);
  batch_add(&b, 50U, 13U, 2U, 2U, (phase == 1U) ? COL_CYAN : COL_BORDER);
  batch_add(&b, 57U, 7U, 2U, 2U, (phase == 2U) ? COL_AMBER : COL_DIM);
  batch_add(&b, 45U, 16U, 1U, 1U, (phase == 3U) ? COL_WHITE : COL_BORDER);
  batch_add(&b, 59U, 15U, 2U, 2U, ((phase & 1U) == 0U) ? COL_CYAN : COL_DIM);

  /* Seconds progress bar under big clock (x=14..113, y=58..63: clear full tip height first!) */
  batch_add(&b, 14U, 58U, 100U, 6U, COL_CARD);
  batch_add(&b, 14U, 59U, 100U, 4U, COL_DIM);
  uint8_t sec_w = (uint8_t)(((uint32_t)disp_s * 100U) / 59U);
  if (sec_w > 100U) sec_w = 100U;
  if (sec_w > 0U) {
    batch_add(&b, 14U, 59U, sec_w, 4U, COL_CYAN);
  }
  uint8_t tip_x = (uint8_t)(14U + ((sec_w > 2U) ? (sec_w - 2U) : 0U));
  batch_add(&b, tip_x, 58U, 2U, 6U, COL_WHITE);
  batch_submit(hud, &b, false);

  /* Giant 2x pixel clock digits "HH:MM" @ (34, 30) with blinking ':' */
  bool time_chg = force_all || (disp_h != hud->prev_saver_hour) ||
                  (disp_m != hud->prev_saver_min) ||
                  (l->rtc_valid != hud->prev_saver_rtc_valid);
  bool info_chg = force_all || (compute_saver_hash(hud) != hud->prev_saver_hash);
  if (time_chg) {
    char hh[3], mm[3];
    fmt_u8_zero2(disp_h, hh);
    fmt_u8_zero2(disp_m, mm);
    q_text(hud, 34U, 30U, hh, COL_WHITE, COL_CARD, 2U, false, false);
    q_text(hud, 70U, 30U, mm, COL_WHITE, COL_CARD, 2U, false, false);
    hud->prev_saver_hour      = disp_h;
    hud->prev_saver_min       = disp_m;
    hud->prev_saver_rtc_valid = l->rtc_valid;
  }
  q_text(hud, 58U, 30U, ":", ((phase & 1U) == 0U) ? COL_CYAN : COL_DIM,
         COL_CARD, 2U, false, !info_chg);
  hud->prev_saver_sec = disp_s;

  if (info_chg) {
    draw_screensaver_sysinfo(hud, true);
  }
}

/* Draw static chrome of fullscreen interactive 82-key Hall calibration screen (128x128) */
static void draw_cal_static(pixel_hud_t *hud)
{
  q_fill(hud, 0U, 0U, 128U, 128U, COL_BG, false);

  rect_batch_t b;
  batch_init(&b);
  /* Header bar (y=0..18) + footer bar (y=111..127) */
  batch_add(&b, 0U, 0U, 128U, 18U, COL_CARD);
  batch_add(&b, 0U, 18U, 128U, 1U, COL_BORDER);
  batch_add(&b, 0U, 111U, 128U, 1U, COL_BORDER);
  batch_add(&b, 0U, 112U, 128U, 16U, COL_CARD);

  /* Strength bar outer frame (x=9..118, y=90..101) */
  batch_add(&b, 9U, 89U, 110U, 12U, COL_BORDER);
  batch_add(&b, 10U, 90U, 108U, 10U, COL_DIM);

  /* Footer overall progress bar frame (x=6..66, y=117..122) */
  batch_add(&b, 6U, 117U, 60U, 6U, COL_DIM);
  batch_submit(hud, &b, false);

  q_text(hud, 4U, 3U, "HALL CAL", COL_CYAN, COL_CARD, 1U, false, false);
  q_text(hud, 10U, 75U, "FORCE", COL_MUTED, COL_BG, 1U, false, false);
  q_text(hud, 74U, 114U, "KNOB:ESC", COL_MUTED, COL_CARD, 1U, false, false);
}

/* Draw dynamic calibration elements: counter, giant keycap symbol, live strength bar, progress */
static void draw_cal_dynamic(pixel_hud_t *hud, bool force_all)
{
  pixel_hud_live_t const *l = &hud->live;
  bool cnt_chg = force_all || (l->cal_done_count != hud->prev_cal_done);
  bool key_chg = force_all || (l->cal_key_done != hud->prev_cal_key_done) ||
                 (strncmp(l->cal_label, hud->prev_cal_label, 4U) != 0);
  bool pct_chg = force_all || (l->cal_strength_pct != hud->prev_cal_pct) || key_chg;

  if (!cnt_chg && !key_chg && !pct_chg) {
    return;
  }

  /* Keycap card + strength bar + footer progress bar in 1 atomic batch */
  rect_batch_t b;
  batch_init(&b);

  if (key_chg) {
    uint16_t kborder = l->cal_key_done ? COL_MINT
                     : ((l->cal_strength_pct > 0U) ? COL_CYAN : COL_BORDER);
    batch_add(&b, 14U, 23U, 100U, 48U, kborder);
    batch_add(&b, 16U, 25U, 96U, 44U, COL_CARD);
    /* Subtle inner keycap top bevel */
    batch_add(&b, 18U, 26U, 92U, 2U, l->cal_key_done ? COL_MINT : COL_BORDER);
  }

  if (pct_chg) {
    batch_add(&b, 10U, 90U, 108U, 10U, COL_DIM);
    uint8_t fw = (uint8_t)(((uint32_t)l->cal_strength_pct * 108U) / 100U);
    if (fw > 108U) fw = 108U;
    if (fw > 0U) {
      uint16_t scol = l->cal_key_done ? COL_MINT
                    : ((l->cal_strength_pct >= 65U) ? COL_AMBER : COL_CYAN);
      batch_add(&b, 10U, 90U, fw, 10U, scol);
    }
    /* Calibrated depth marker tick at ~35% */
    batch_add(&b, 48U, 89U, 2U, 12U, COL_WHITE);
  }

  if (cnt_chg) {
    uint8_t tot = (l->cal_total_keys > 0U) ? l->cal_total_keys : 82U;
    uint8_t pw  = (uint8_t)(((uint32_t)l->cal_done_count * 60U) / tot);
    if (pw > 60U) pw = 60U;
    batch_add(&b, 6U, 117U, 60U, 6U, COL_DIM);
    if (pw > 0U) {
      batch_add(&b, 6U, 117U, pw, 6U, COL_MINT);
    }
  }
  bool any_text = cnt_chg || key_chg || pct_chg;
  batch_submit(hud, &b, !any_text);

  if (cnt_chg) {
    uint8_t tot = (l->cal_total_keys > 0U) ? l->cal_total_keys : 82U;
    char cstr[6];
    char d2[3], t2[3];
    fmt_u8_zero2(l->cal_done_count, d2);
    fmt_u8_zero2(tot, t2);
    cstr[0] = d2[0]; cstr[1] = d2[1]; cstr[2] = '/'; cstr[3] = t2[0]; cstr[4] = t2[1]; cstr[5] = '\0';
    q_text(hud, 92U, 3U, cstr, COL_MINT, COL_CARD, 1U, false, !(key_chg || pct_chg));
    hud->prev_cal_done = l->cal_done_count;
  }

  if (key_chg) {
    char lbl[5];
    memcpy(lbl, l->cal_label, 5U);
    lbl[4] = '\0';
    size_t len   = strlen(lbl);
    uint8_t sc   = (len <= 1U) ? 3U : 2U;
    uint8_t tw   = (uint8_t)(len * 6U * sc);
    uint8_t th   = (uint8_t)(12U * sc);
    uint8_t tx   = (tw < 96U) ? (uint8_t)(16U + ((96U - tw) / 2U)) : 18U;
    uint8_t ty   = (th < 44U) ? (uint8_t)(25U + ((44U - th) / 2U)) : 28U;
    uint16_t kcol = l->cal_key_done ? COL_MINT : COL_WHITE;
    q_text(hud, tx, ty, lbl, kcol, COL_CARD, sc, false, !pct_chg);
    memcpy(hud->prev_cal_label, lbl, 5U);
    hud->prev_cal_key_done = l->cal_key_done;
  }

  if (pct_chg) {
    char pstr[5];
    if (l->cal_key_done && l->cal_strength_pct >= 35U) {
      memcpy(pstr, " OK!", 5U);
    } else {
      fmt_u8_pct(l->cal_strength_pct, pstr);
    }
    q_text(hud, 94U, 75U, pstr, l->cal_key_done ? COL_MINT : COL_CYAN, COL_BG, 1U, false, true);
    hud->prev_cal_pct = l->cal_strength_pct;
  }
}

static void render_wipe_step(pixel_hud_t *hud)
{
  /* 3-step horizontal pixel-art cyan scanline wipe across y=19..110 (zero underflow, zero residue) */
  uint8_t step = hud->wipe_step;
  rect_batch_t b;
  batch_init(&b);

  if (hud->wipe_dir >= 0) {
    if (step == 1U) {
      batch_add(&b, 0U, 19U, 44U, 92U, COL_BG);
      batch_add(&b, 41U, 19U, 3U, 92U, COL_CYAN);
    } else if (step == 2U) {
      batch_add(&b, 41U, 19U, 45U, 92U, COL_BG);
      batch_add(&b, 83U, 19U, 3U, 92U, COL_CYAN);
    } else {
      batch_add(&b, 83U, 19U, 45U, 92U, COL_BG);
    }
  } else {
    if (step == 1U) {
      batch_add(&b, 84U, 19U, 44U, 92U, COL_BG);
      batch_add(&b, 84U, 19U, 3U, 92U, COL_CYAN);
    } else if (step == 2U) {
      batch_add(&b, 42U, 19U, 45U, 92U, COL_BG);
      batch_add(&b, 42U, 19U, 3U, 92U, COL_CYAN);
    } else {
      batch_add(&b, 0U, 19U, 45U, 92U, COL_BG);
    }
  }
  batch_submit(hud, &b, true);
}

static void start_page_transition(pixel_hud_t *hud, uint8_t new_page, int8_t dir)
{
  hud->page               = (uint8_t)(new_page % PIXEL_PAGE_COUNT);
  hud->wipe_step          = 1U;
  hud->wipe_dir           = dir;
  hud->full_redraw_needed = true;
}

void pixel_hud_init(pixel_hud_t *hud, tk75_persist_cfg_t *cfg,
                    hud_send_frame_fn_t send_fn, void *send_ctx)
{
  if (hud == NULL) {
    return;
  }
  memset(hud, 0, sizeof(*hud));
  hud->cfg                = cfg;
  hud->send_fn            = send_fn;
  hud->send_ctx           = send_ctx;
  hud->page               = PIXEL_PAGE_HOME;
  hud->applied_tft_brt    = 0xFFU;
  hud->macro_play_req     = 0xFFU;
  hud->full_redraw_needed = true;
}

bool pixel_hud_is_active(pixel_hud_t const *hud)
{
  return (hud != NULL) && hud->detected && !hud->stock_ui_mode;
}

bool pixel_hud_is_menu_open(pixel_hud_t const *hud)
{
  return (hud != NULL) && hud->menu_open;
}

void pixel_hud_on_rx(pixel_hud_t *hud, uart7_rx_frame_t const *frame)
{
  if (hud == NULL || frame == NULL) {
    return;
  }
  if (frame->cmd >= 0xA0U && frame->cmd <= 0xA6U) {
    hud->awaiting_ack = false;
  }
  if (frame->cmd == 0xA5U && frame->payload_len >= 3U && frame->payload[0] == 0x00U) {
    hud->patch_major = frame->payload[1];
    hud->patch_minor = frame->payload[2];
    if (!hud->detected) {
      hud->detected           = true;
      hud->stock_ui_mode      = false;
      hud->full_redraw_needed = true;
      hud->tft_brt_dirty      = true;
      hud->applied_tft_brt    = 0xFFU;
      hud->brt_reapply_left   = 3U;
      hud->brt_reapply_ms     = 0U;
      uint8_t on = 1U;
      (void)enqueue_op(hud, PIXEL_HUD_CMD_MANUAL, &on, 1U);
    }
  }
}

void pixel_hud_set_live(pixel_hud_t *hud, pixel_hud_live_t const *live)
{
  if (hud == NULL || live == NULL) {
    return;
  }
  hud->live = *live;
}

void pixel_hud_mark_config_dirty(pixel_hud_t *hud)
{
  if (hud == NULL) {
    return;
  }
  hud->applied_tft_brt    = 0xFFU;
  hud->tft_brt_dirty      = true;
  hud->body_redraw_needed = true;
}

void pixel_hud_open_menu(pixel_hud_t *hud, uint32_t now_ms)
{
  if (hud == NULL || !hud->detected) {
    return;
  }
  if (hud->stock_ui_mode) {
    hud->stock_ui_mode = false;
    uint8_t on = 1U;
    (void)enqueue_op(hud, PIXEL_HUD_CMD_MANUAL, &on, 1U);
    q_fill(hud, 0U, 0U, 128U, 128U, COL_BG, false);
  }
  hud->screensaver_active = false;
  hud->menu_open          = true;
  hud->editing            = false;
  hud->focus_row          = 1U; /* Jump straight to TFT brightness slider for 1-click access */
  hud->last_input_ms      = now_ms;
  start_page_transition(hud, PIXEL_PAGE_BRIGHTNESS, 1);
}

void pixel_hud_close_menu(pixel_hud_t *hud)
{
  if (hud == NULL) {
    return;
  }
  if (!hud->menu_open && hud->page == PIXEL_PAGE_HOME) {
    return;
  }
  hud->menu_open = false;
  hud->editing   = false;
  hud->focus_row = 0U;
  if (!hud->stock_ui_mode) {
    start_page_transition(hud, PIXEL_PAGE_HOME, -1);
  }
}

static bool adjust_focused_item(pixel_hud_t *hud, int8_t dir)
{
  tk75_persist_cfg_t *c = hud->cfg;
  if (c == NULL) {
    return false;
  }

  if (hud->page == PIXEL_PAGE_BRIGHTNESS) {
    if (hud->focus_row == 1U) {
      int32_t v = (int32_t)c->tft_brightness + ((int32_t)dir * 10);
      if (v < 0) v = 0;
      if (v > 100) v = 100;
      c->tft_brightness = (uint8_t)v;
      hud->tft_brt_dirty = true;
      return true;
    }
    if (hud->focus_row == 2U) {
      int32_t p = (int32_t)u8_to_pct10(c->rgb_brightness, c->rgb_enabled) + ((int32_t)dir * 10);
      if (p < 0) p = 0;
      if (p > 100) p = 100;
      c->rgb_brightness = pct10_to_u8((uint8_t)p);
      c->rgb_enabled    = (p > 0) ? 1U : 0U;
      return true;
    }
    if (hud->focus_row == 3U) {
      int32_t p = (int32_t)u8_to_pct10(c->side_brightness, c->side_enabled) + ((int32_t)dir * 10);
      if (p < 0) p = 0;
      if (p > 100) p = 100;
      c->side_brightness = pct10_to_u8((uint8_t)p);
      c->side_enabled    = (p > 0) ? 1U : 0U;
      return true;
    }
    if (hud->focus_row == 4U) {
      int32_t cur = (c->caps_side_enabled == 0U)
                      ? 0
                      : (1 + (int32_t)(c->caps_side_effect % CAPS_SIDE_FX_COUNT));
      int32_t nxt = (cur + dir + (int32_t)(CAPS_SIDE_FX_COUNT + 1U)) % (int32_t)(CAPS_SIDE_FX_COUNT + 1U);
      if (nxt == 0) {
        c->caps_side_enabled = 0U;
      } else {
        c->caps_side_enabled = 1U;
        c->caps_side_effect  = (uint8_t)(nxt - 1);
      }
      return true;
    }
  } else if (hud->page == PIXEL_PAGE_LIGHTING) {
    if (hud->focus_row == 1U) {
      int32_t cur = (c->rgb_enabled != 0U) ? (int32_t)c->rgb_effect : 0;
      int32_t fx  = (cur + dir + 7) % 7;
      c->rgb_effect  = (uint8_t)fx;
      c->rgb_enabled = (fx != 0) ? 1U : 0U;
      if (fx != 0 && c->rgb_brightness == 0U) {
        c->rgb_brightness = 128U;
      }
      return true;
    }
    if (hud->focus_row == 2U) {
      int32_t idx = ((int32_t)find_nearest_color_preset(c->rgb_hue, c->rgb_sat) + dir +
                     (int32_t)COLOR_PRESET_COUNT) % (int32_t)COLOR_PRESET_COUNT;
      c->rgb_hue = k_color_presets[idx].hue;
      c->rgb_sat = k_color_presets[idx].sat;
      return true;
    }
    if (hud->focus_row == 3U) {
      int32_t cur = (c->side_enabled != 0U) ? (int32_t)(c->side_effect % 4U) : 0;
      int32_t fx  = (cur + dir + 4) % 4;
      c->side_effect  = (uint8_t)fx;
      c->side_enabled = (fx != 0) ? 1U : 0U;
      if (fx != 0 && c->side_brightness == 0U) {
        c->side_brightness = 128U;
      }
      return true;
    }
    if (hud->focus_row == 4U) {
      int32_t idx = ((int32_t)find_nearest_color_preset(c->side_hue, c->side_sat) + dir +
                     (int32_t)COLOR_PRESET_COUNT) % (int32_t)COLOR_PRESET_COUNT;
      c->side_hue = k_color_presets[idx].hue;
      c->side_sat = k_color_presets[idx].sat;
      return true;
    }
  } else if (hud->page == PIXEL_PAGE_SWITCH) {
    if (hud->focus_row == 1U) {
      int32_t um = (int32_t)c->hall_actuation_um + ((int32_t)dir * 100);
      if (um < 100) um = 100;
      if (um > 3900) um = 3900;
      c->hall_actuation_um = (uint16_t)um;
      return true;
    }
    if (hud->focus_row == 2U) {
      static uint16_t const k_rt_steps[5] = { 0U, 200U, 400U, 800U, 1200U };
      int32_t idx = 0;
      if (c->hall_rt_enabled != 0U) {
        idx = 1;
        for (int32_t i = 1; i < 5; i++) {
          if (c->hall_rt_down_um >= k_rt_steps[i]) {
            idx = i;
          }
        }
      }
      idx += dir;
      if (idx < 0) idx = 0;
      if (idx > 4) idx = 4;
      if (idx == 0) {
        c->hall_rt_enabled = 0U;
      } else {
        c->hall_rt_enabled = 1U;
        c->hall_rt_down_um = k_rt_steps[idx];
        c->hall_rt_up_um   = k_rt_steps[idx];
      }
      return true;
    }
    if (hud->focus_row == 3U) {
      int32_t m = ((int32_t)c->hall_socd_mode + dir + 3) % 3;
      c->hall_socd_mode = (uint8_t)m;
      return true;
    }
  } else if (hud->page == PIXEL_PAGE_MACRO) {
    if (hud->focus_row == 1U || hud->focus_row == 2U) {
      int32_t spd = (int32_t)macro_player_normalize_speed(c->macro_speed_idx) + dir;
      if (spd < (int32_t)MACRO_SPEED_FAST) spd = (int32_t)MACRO_SPEED_FAST;
      if (spd > (int32_t)MACRO_SPEED_SAFE) spd = (int32_t)MACRO_SPEED_SAFE;
      c->macro_speed_idx = (uint8_t)spd;
      return true;
    }
    if (hud->focus_row == 3U) {
      int32_t slot = ((int32_t)hud->macro_sel_slot + (int32_t)dir + 5) % 5;
      hud->macro_sel_slot = (uint8_t)slot;
      return false;
    }
  } else if (hud->page == PIXEL_PAGE_SYSTEM) {
    if (hud->focus_row == 1U) {
      int32_t p = (int32_t)u8_to_pct10(c->rgb_speed, 1U) + ((int32_t)dir * 10);
      if (p < 10) p = 10;
      if (p > 100) p = 100;
      uint8_t sp = pct10_to_u8((uint8_t)p);
      c->rgb_speed  = sp;
      c->side_speed = sp;
      return true;
    }
    if (hud->focus_row == 2U) {
      c->mac_mode ^= 1U;
      return true;
    }
    if (hud->focus_row == 3U) {
      c->tft_screensaver ^= 1U;
      return true;
    }
    if (hud->focus_row == 4U) {
      hud->stock_ui_mode = !hud->stock_ui_mode;
      if (hud->stock_ui_mode) {
        /* Clear entire 128x128 screen to stock charcoal (0x18E4) before handing back to stock UI */
        q_fill(hud, 0U, 0U, 128U, 128U, 0x18E4U, true);
        uint8_t off = 0U;
        (void)enqueue_op(hud, PIXEL_HUD_CMD_MANUAL, &off, 1U);
        hud->menu_open          = false;
        hud->editing            = false;
        hud->screensaver_active = false;
        hud->wipe_step          = 0U;
        hud->full_redraw_needed = false;
        hud->body_redraw_needed = false;
      } else {
        uint8_t on = 1U;
        (void)enqueue_op(hud, PIXEL_HUD_CMD_MANUAL, &on, 1U);
        q_fill(hud, 0U, 0U, 128U, 128U, COL_BG, false);
        hud->full_redraw_needed = true;
      }
      return false;
    }
  }
  return false;
}

bool pixel_hud_input(pixel_hud_t *hud, hud_menu_act_t act, uint32_t now_ms)
{
  if (hud == NULL || !hud->detected || act == HUD_MENU_ACT_NONE) {
    return false;
  }
  hud->last_input_ms = now_ms;

  if (!hud->menu_open) {
    pixel_hud_open_menu(hud, now_ms);
    return false;
  }

  int8_t dir = (act == HUD_MENU_ACT_NEXT) ? 1 : ((act == HUD_MENU_ACT_PREV) ? -1 : 0);

  if (hud->editing) {
    if (act == HUD_MENU_ACT_OK) {
      if (hud->page == PIXEL_PAGE_MACRO && hud->focus_row == 3U && hud->cfg != NULL) {
        uint8_t s = hud->macro_sel_slot % 5U;
        if (macro_recorder_slot_has_data(hud->cfg->macros, s)) {
          hud->macro_play_req = s;
        }
      }
      hud->editing            = false;
      hud->body_redraw_needed = true;
      return false;
    }
    bool chg = adjust_focused_item(hud, dir);
    if (!hud->stock_ui_mode) {
      hud->body_redraw_needed = true;
    }
    return chg;
  }

  if (act == HUD_MENU_ACT_OK) {
    if (hud->page == PIXEL_PAGE_HOME) {
      pixel_hud_close_menu(hud);
      return false;
    }
    if (hud->focus_row == 0U) {
      hud->focus_row          = 1U;
      hud->body_redraw_needed = true;
    } else if (hud->page == PIXEL_PAGE_SWITCH && hud->focus_row == 4U) {
      /* 1-click launch interactive 82-key Hall calibration */
      hud->cal_start_req      = true;
      hud->menu_open          = false;
      hud->editing            = false;
      hud->focus_row          = 0U;
      hud->page               = PIXEL_PAGE_HOME;
      hud->screensaver_active = false;
    } else {
      hud->editing            = true;
      hud->body_redraw_needed = true;
    }
    return false;
  }

  /* Not editing: PREV / NEXT navigates tab bar or rows */
  if (hud->focus_row == 0U || hud->page == PIXEL_PAGE_HOME) {
    int32_t next_p = ((int32_t)hud->page + (int32_t)PIXEL_PAGE_COUNT + (int32_t)dir) % (int32_t)PIXEL_PAGE_COUNT;
    hud->focus_row = 0U;
    start_page_transition(hud, (uint8_t)next_p, dir);
  } else {
    int32_t max_r  = (hud->page == PIXEL_PAGE_BRIGHTNESS ||
                      hud->page == PIXEL_PAGE_LIGHTING ||
                      hud->page == PIXEL_PAGE_SWITCH ||
                      hud->page == PIXEL_PAGE_SYSTEM) ? 4 : 3;
    int32_t next_r = (int32_t)hud->focus_row + dir;
    if (next_r < 0) next_r = max_r;
    if (next_r > max_r) next_r = 0;
    hud->focus_row          = (uint8_t)next_r;
    hud->body_redraw_needed = true;
  }
  return false;
}

static void pump_tx_queue(pixel_hud_t *hud, uint32_t now_ms)
{
  if (hud->q_count == 0U || hud->send_fn == NULL) {
    return;
  }
  if (hud->awaiting_ack && (uint32_t)(now_ms - hud->last_tx_ms) < ACK_TIMEOUT_MS) {
    return;
  }
  pixel_hud_op_t const *op = &hud->q[hud->q_head];
  uart7_tx_frame_t frame = {
    .cmd         = op->cmd,
    .payload_len = op->len,
    .payload     = op->data,
  };
  if (hud->send_fn(&frame, hud->send_ctx)) {
    hud->q_head       = (uint8_t)((hud->q_head + 1U) % PIXEL_HUD_QUEUE_CAP);
    hud->q_count--;
    hud->awaiting_ack = true;
    hud->last_tx_ms   = now_ms;
  }
}

void pixel_hud_tick(pixel_hud_t *hud, uint32_t now_ms)
{
  if (hud == NULL || hud->cfg == NULL || hud->send_fn == NULL) {
    return;
  }

  /* Phase 1: Probe U24 for custom patch (cmd=0x25 -> 0xA5) until detected */
  if (!hud->detected) {
    if ((uint32_t)(now_ms - hud->last_probe_ms) >= PROBE_INTERVAL_MS) {
      hud->last_probe_ms = now_ms;
      uart7_tx_frame_t probe = { .cmd = PIXEL_HUD_CMD_VERSION, .payload_len = 0U, .payload = NULL };
      (void)hud->send_fn(&probe, hud->send_ctx);
    }
    return;
  }

  /* Drain queued draw ops first */
  pump_tx_queue(hud, now_ms);
  if (hud->stock_ui_mode) {
    return;
  }

  /* Menu auto-timeout */
  if (hud->menu_open && (uint32_t)(now_ms - hud->last_input_ms) >= MENU_TIMEOUT_MS) {
    pixel_hud_close_menu(hud);
  }

  /* Post-boot brightness + display flush re-assert: U24 stock boot sequence turns backlight
   * up to 100% and finishes ST7735S init ~0.5..1.5s after power-on, so re-assert saved PWM
   * and trigger a clean FB_DIRTY flush (0x24 [0xFF]) at +300ms, +900ms, and +1800ms. */
  if (hud->brt_reapply_left > 0U) {
    if (hud->brt_reapply_ms == 0U) {
      hud->brt_reapply_ms = now_ms + 300U;
    } else if ((int32_t)(now_ms - hud->brt_reapply_ms) >= 0) {
      hud->applied_tft_brt = 0xFFU;
      hud->tft_brt_dirty   = true;
      if (hud->brt_reapply_left == 3U && !hud->menu_open && !hud->screensaver_active &&
          !hud->live.macro_recording && !hud->live.macro_reviewing &&
          !hud->live.cal_active && !hud->live.any_actuated) {
        /* One extra full repaint after U24 boot settles guarantees zero missing elements */
        hud->full_redraw_needed = true;
      } else if (hud->q_count < (PIXEL_HUD_QUEUE_CAP - 3U)) {
        uint8_t fl = 0xFFU;
        (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, &fl, 1U);
      }
      hud->brt_reapply_left--;
      if (hud->brt_reapply_left == 2U) {
        hud->brt_reapply_ms = now_ms + 600U;
      } else if (hud->brt_reapply_left == 1U) {
        hud->brt_reapply_ms = now_ms + 900U;
      } else {
        hud->brt_reapply_ms = 0U;
      }
    }
  }

  /* Apply TFT brightness whenever changed (after initial full page is queued so it follows first flush) */
  if (!hud->full_redraw_needed && hud->tft_brt_dirty && hud->q_count < (PIXEL_HUD_QUEUE_CAP - 2U)) {
    hud->tft_brt_dirty = false;
    if (hud->cfg->tft_brightness != hud->applied_tft_brt) {
      hud->applied_tft_brt = hud->cfg->tft_brightness;
      q_brightness_pct(hud, hud->applied_tft_brt);
      pump_tx_queue(hud, now_ms);
    }
  }

  /* Only enqueue new visual frames when queue is nearly empty (prevents lag buildup) */
  if (hud->q_count > 2U) {
    return;
  }

  /* Fullscreen interactive Hall calibration takeover */
  if (hud->live.cal_active) {
    hud->screensaver_active = false;
    if (!hud->prev_cal_active) {
      hud->prev_cal_active = true;
      hud->prev_cal_done   = 0xFFU;
      hud->prev_cal_pct    = 0xFFU;
      hud->last_anim_ms    = now_ms;
      draw_cal_static(hud);
      draw_cal_dynamic(hud, true);
      pump_tx_queue(hud, now_ms);
      return;
    }
    if ((uint32_t)(now_ms - hud->last_anim_ms) >= ANIM_PERIOD_MS) {
      hud->last_anim_ms = now_ms;
      draw_cal_dynamic(hud, false);
      pump_tx_queue(hud, now_ms);
    }
    return;
  } else if (hud->prev_cal_active) {
    hud->prev_cal_active    = false;
    hud->wipe_step          = 0U;
    hud->page               = PIXEL_PAGE_HOME;
    hud->full_redraw_needed = true;
  }

  /* Fullscreen pixel-art macro text editor (review & edit before final Fn confirm) */
  if (hud->live.macro_reviewing) {
    hud->screensaver_active = false;
    hud->prev_macro_rec     = false;
    if (!hud->prev_macro_rev) {
      hud->prev_macro_rev   = true;
      hud->edit_blink_on    = true;
      hud->last_anim_ms     = now_ms;
      uint8_t cur_row       = (uint8_t)(hud->live.macro_edit_cursor / EDIT_VIEW_COLS);
      hud->edit_scroll_row  = (cur_row >= EDIT_VIEW_ROWS) ? (uint8_t)(cur_row - (EDIT_VIEW_ROWS - 1U)) : 0U;
      hud->prev_edit_rev    = hud->live.macro_edit_rev;
      hud->prev_edit_cursor = hud->live.macro_edit_cursor;
      hud->edit_dirty_mask  = 0x3FU;
      draw_macro_edit_static(hud);
      step_macro_edit_hud(hud, now_ms);
      pump_tx_queue(hud, now_ms);
      return;
    }
    step_macro_edit_hud(hud, now_ms);
    pump_tx_queue(hud, now_ms);
    return;
  } else if (hud->prev_macro_rev) {
    hud->prev_macro_rev     = false;
    hud->edit_dirty_mask    = 0U;
    hud->wipe_step          = 0U;
    hud->full_redraw_needed = true;
  }

  /* Fullscreen pixel-art macro recording takeover */
  if (hud->live.macro_recording) {
    hud->screensaver_active = false;
    if (!hud->prev_macro_rec) {
      hud->prev_macro_rec = true;
      hud->rec_anim_phase = 0U;
      hud->prev_rec_bytes = 0xFFFFU;
      hud->last_anim_ms   = now_ms;
      draw_macro_rec_static(hud);
      draw_macro_rec_anim(hud, true);
      pump_tx_queue(hud, now_ms);
      return;
    }
    bool bytes_chg = (hud->live.macro_rec_bytes != hud->prev_rec_bytes);
    if (bytes_chg || (uint32_t)(now_ms - hud->last_anim_ms) >= 120U) {
      hud->last_anim_ms   = now_ms;
      hud->rec_anim_phase = (uint8_t)((hud->rec_anim_phase + 1U) & 3U);
      draw_macro_rec_anim(hud, false);
      pump_tx_queue(hud, now_ms);
    }
    return;
  } else if (hud->prev_macro_rec) {
    hud->prev_macro_rec     = false;
    hud->wipe_step          = 0U;
    hud->full_redraw_needed = true;
  }

  /* Pixel-art idle screensaver (Clock & System Info); instant wake on typing/input/VIA disable.
   * Do not gate on raw max_travel_um because warm Hall sensors can sit at 300..450um below actuation. */
  bool saver_want = (hud->cfg->tft_screensaver != 0U) && !hud->menu_open &&
                    (hud->page == PIXEL_PAGE_HOME) &&
                    (hud->live.idle_ms >= SCREENSAVER_IDLE_MS) &&
                    !hud->live.any_actuated;
  if (saver_want) {
    if (!hud->screensaver_active) {
      hud->screensaver_active   = true;
      hud->saver_phase          = 0U;
      hud->prev_saver_hour      = 0xFFU;
      hud->prev_saver_min       = 0xFFU;
      hud->prev_saver_sec       = 0xFFU;
      hud->prev_saver_rtc_valid = false;
      hud->last_anim_ms         = now_ms;
      draw_screensaver_static(hud);
      draw_screensaver_anim(hud, true);
      pump_tx_queue(hud, now_ms);
      return;
    }
    if ((uint32_t)(now_ms - hud->last_anim_ms) >= 500U) {
      hud->last_anim_ms = now_ms;
      hud->saver_phase  = (uint8_t)((hud->saver_phase + 1U) & 3U);
      draw_screensaver_anim(hud, false);
      pump_tx_queue(hud, now_ms);
    }
    return;
  } else if (hud->screensaver_active) {
    hud->screensaver_active = false;
    hud->wipe_step          = 0U;
    hud->full_redraw_needed = true;
  }

  /* Smooth 3-step horizontal pixel wipe on page switch */
  if (hud->wipe_step > 0U) {
    if ((uint32_t)(now_ms - hud->last_anim_ms) >= 16U) {
      hud->last_anim_ms = now_ms;
      render_wipe_step(hud);
      hud->wipe_step++;
      if (hud->wipe_step > 3U) {
        hud->wipe_step = 0U;
      }
    }
    pump_tx_queue(hud, now_ms);
    return;
  }

  if (hud->full_redraw_needed || hud->body_redraw_needed) {
    hud->full_redraw_needed = false;
    hud->body_redraw_needed = false;
    render_full_page(hud);
    pump_tx_queue(hud, now_ms);
    return;
  }

  /* Live HOME page incremental updates (~35 FPS smooth switch animation) */
  if (hud->page == PIXEL_PAGE_HOME && (uint32_t)(now_ms - hud->last_anim_ms) >= ANIM_PERIOD_MS) {
    hud->last_anim_ms = now_ms;

    uint16_t disp_travel = update_display_travel(hud);
    uint8_t  target_px   = (uint8_t)(((uint32_t)disp_travel * PLUNGER_MAX_PX) / 4000U);
    if (target_px > PLUNGER_MAX_PX) target_px = PLUNGER_MAX_PX;

    /* Spring-eased motion: fast press down, smooth snap-back up */
    uint8_t next_px = hud->anim_plunger_px;
    if (target_px > next_px) {
      uint8_t step = (uint8_t)((target_px - next_px + 1U) / 2U);
      next_px = (uint8_t)(next_px + ((step < 2U) ? 2U : step));
      if (next_px > target_px) next_px = target_px;
    } else if (target_px < next_px) {
      uint8_t step = (uint8_t)((next_px - target_px + 1U) / 2U);
      uint8_t dec  = (step < 2U) ? 2U : step;
      next_px = (next_px > dec) ? (uint8_t)(next_px - dec) : 0U;
      if (next_px < target_px) next_px = target_px;
    }

    bool hdr_chg   = (compute_header_hash(hud) != hud->prev_header_hash);
    bool sw_chg    = (next_px != hud->anim_plunger_px) || (hud->live.any_actuated != hud->prev_actuated);
    uint16_t td    = (disp_travel > 0U)
                       ? (uint16_t)((disp_travel + 50U) / 100U)
                       : (uint16_t)((hud->cfg->hall_actuation_um + 50U) / 100U);
    bool stats_chg = (hud->live.wpm != hud->prev_wpm) || (td != hud->prev_travel_disp) ||
                     (compute_badges_hash(hud) != hud->prev_badges_hash);

    if (hdr_chg) {
      draw_home_header(hud);
    }
    if (sw_chg) {
      draw_switch_animation(hud, next_px, hud->live.any_actuated, !stats_chg);
    }
    if (stats_chg) {
      draw_home_right_stats(hud, false, true);
    } else if (hdr_chg && !sw_chg) {
      uint8_t f = 0xFFU;
      (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, &f, 1U);
    }
    pump_tx_queue(hud, now_ms);
  }
}
