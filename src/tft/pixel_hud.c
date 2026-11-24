#include "pixel_hud.h"
#include "macro_player.h"
#include "macro_recorder.h"
#include "rgb_effects.h"
#include <string.h>

#define PROBE_INTERVAL_MS   100U
#define ACK_TIMEOUT_MS      150U
#define ANIM_PERIOD_MS      28U
#define MENU_TIMEOUT_MS     15000U
#define SCREENSAVER_IDLE_MS 30000U
#define PLUNGER_MAX_PX      18U
#define MAX_BATCH_RECTS     15U

/* Theme descriptor: distinct palette + structural chrome style per skin */
typedef struct {
  char const *name;         /* Fixed 6-char menu display label */
  uint16_t    bg;
  uint16_t    card;
  uint16_t    border;
  uint16_t    dim;
  uint16_t    accent;       /* Primary accent */
  uint16_t    good;         /* Actuated / ON / Active */
  uint16_t    warn;         /* Edit / Rapid Trigger / Warm */
  uint16_t    alert;        /* Low Battery / Caps / Rec */
  uint16_t    muted;        /* Secondary label */
  uint16_t    text;         /* Primary bright text */
  uint16_t    keycap;
  uint16_t    keytop;
  uint16_t    spring;
  uint16_t    housing;
  uint16_t    header_bg;    /* Menu header fill when unfocused */
  uint16_t    header_focus; /* Menu header fill when tab bar focused */
  uint16_t    focus_bg;     /* Focused menu row interior fill */
  uint16_t    cursor_fg;    /* Text color on accent-filled pills/cursors */
  uint8_t     id;           /* tft_theme_t */
} hud_theme_t;

static hud_theme_t const k_themes[TFT_THEME_COUNT] = {
  /* 0: PIXEL (Obsidian Cyber) */
  {
    .name         = "PIXEL ",
    .bg           = 0x0862U, /* #0B0F17 Deep Obsidian */
    .card         = 0x10E5U, /* #151C2C Dark Slate */
    .border       = 0x29AAU, /* #2A3550 Crisp Pixel Bevel */
    .dim          = 0x2127U, /* #202638 Cavity / Slider Track */
    .accent       = 0x3F5FU, /* #38E8FF Electric Cyan */
    .good         = 0x37D3U, /* #36F998 Neon Mint */
    .warn         = 0xFDC6U, /* #FFB830 Warm Amber */
    .alert        = 0xF9C9U, /* #FF3B4B Coral Alert */
    .muted        = 0x63B1U, /* #64748B Muted Label */
    .text         = 0xFFDFU, /* #F8FAFC Crisp White */
    .keycap       = 0x4A8EU,
    .keytop       = 0x8474U,
    .spring       = 0x94D6U,
    .housing      = 0x31A9U,
    .header_bg    = 0x10E5U,
    .header_focus = 0x29AAU,
    .focus_bg     = 0x10E5U,
    .cursor_fg    = 0x0862U,
    .id           = (uint8_t)TFT_THEME_PIXEL,
  },
  /* 1: STOCK+ (Modified GamaKay Stock Classic with ALL custom features) */
  {
    .name         = "STOCK+",
    .bg           = 0x18E4U, /* #181C20 Authentic Stock Charcoal */
    .card         = 0x2146U, /* #222830 Graphite Panel */
    .border       = 0x3A29U, /* #3A444D Steel Edge */
    .dim          = 0x10A3U, /* #121518 Recessed Track */
    .accent       = 0x353FU, /* #33A6FF GamaKay Electric Blue */
    .good         = 0x2EEBU, /* #28DD58 Emerald Good */
    .warn         = 0xFBC3U, /* #FF7818 GamaKay Signature Orange */
    .alert        = 0xF986U, /* #FF3030 Crimson Alert */
    .muted        = 0x94B2U, /* #949694 Crisp Silver Grey */
    .text         = 0xFFFFU, /* #FFFFFF Pure White */
    .keycap       = 0x4A6AU,
    .keytop       = 0x9CF4U,
    .spring       = 0xAD76U,
    .housing      = 0x2987U,
    .header_bg    = 0x1969U, /* Deep Navy Steel Banner */
    .header_focus = 0x353FU, /* Active Electric Blue Banner */
    .focus_bg     = 0x21ECU, /* Elevated Blue-Slate Selection Bar */
    .cursor_fg    = 0x18E4U,
    .id           = (uint8_t)TFT_THEME_STOCK_PLUS,
  },
  /* 2: TERM (Retro Phosphor CRT Terminal) */
  {
    .name         = "TERM  ",
    .bg           = 0x0020U, /* #000400 Pitch CRT Black */
    .card         = 0x08C1U, /* #081808 Deep Phosphor Tube */
    .border       = 0x1364U, /* #106C20 CRT Grid Green */
    .dim          = 0x0861U, /* #080C08 Unlit Segment */
    .accent       = 0x37ECU, /* #33FF66 Bright Phosphor Green */
    .good         = 0x9FF7U, /* #99FFBB High-Glow Mint */
    .warn         = 0xFD40U, /* #FFA800 Amber CRT Highlight */
    .alert        = 0xF9A8U, /* #FF3440 CRT Alert Red */
    .muted        = 0x2C85U, /* #289028 Dim Phosphor */
    .text         = 0x9FF7U, /* #99FFBB Phosphor Text */
    .keycap       = 0x12A3U,
    .keytop       = 0x37ECU,
    .spring       = 0x2C85U,
    .housing      = 0x0A02U,
    .header_bg    = 0x08C1U,
    .header_focus = 0x1364U,
    .focus_bg     = 0x0902U,
    .cursor_fg    = 0x0020U,
    .id           = (uint8_t)TFT_THEME_TERMINAL,
  },
};

/* Configurable Deep Energy Save presets (minutes; 0 = disabled) */
#define ECO_PRESET_COUNT 7U
static uint8_t const k_eco_presets[ECO_PRESET_COUNT] = { 0U, 1U, 2U, 5U, 10U, 15U, 30U };

typedef struct {
  uint8_t count;
  uint8_t buf[2U + MAX_BATCH_RECTS * 6U];
} rect_batch_t;

static uint8_t normalize_theme_id(uint8_t raw)
{
  return (raw < (uint8_t)TFT_THEME_COUNT) ? raw : (uint8_t)TFT_THEME_PIXEL;
}

static uint8_t normalize_saver_mode(uint8_t raw)
{
  return (raw < (uint8_t)TFT_SAVER_COUNT) ? raw : (uint8_t)TFT_SAVER_OFF;
}

static char const *saver_mode_short_name(uint8_t mode)
{
  static char const *const k_names[TFT_SAVER_COUNT] = {
    "OFF   ", "CLOCK ", "EYES  ", "CAT   ", "MATRIX", "DVD   ", "GIF   ",
  };
  return k_names[normalize_saver_mode(mode)];
}

static hud_theme_t const *active_theme(pixel_hud_t const *hud)
{
  uint8_t id = (hud != NULL && hud->cfg != NULL) ? normalize_theme_id(hud->cfg->tft_theme) : 0U;
  return &k_themes[id];
}

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

static void fmt_u8_zero2(uint8_t v, char out[3])
{
  if (v > 99U) {
    v = 99U;
  }
  out[0] = (char)('0' + (v / 10U));
  out[1] = (char)('0' + (v % 10U));
  out[2] = '\0';
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

static void fmt_eco_timeout(uint8_t mins, char out[7])
{
  if (mins == 0U) {
    memcpy(out, "OFF   ", 7U);
    return;
  }
  if (mins >= 10U) {
    out[0] = (char)('0' + ((mins / 10U) % 10U));
    out[1] = (char)('0' + (mins % 10U));
    memcpy(&out[2], " MIN", 5U);
  } else {
    out[0] = (char)('0' + mins);
    memcpy(&out[1], " MIN ", 6U);
  }
}

static char const *rf_rate_short_name(uint8_t idx)
{
  static char const *const k_rates[4] = { "1000Hz", "500Hz ", "250Hz ", "125Hz " };
  return (idx < 4U) ? k_rates[idx] : k_rates[0];
}

static uint8_t u8_to_pct10(uint8_t val, uint8_t enabled)
{
  if (enabled == 0U || val == 0U) {
    return 0U;
  }
  uint32_t pct     = (((uint32_t)val * 100U) + 127U) / 255U;
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
 *   - USB Charging  (vbus && charging):  Warn/Text lightning bolt + Warn battery outline/fill
 *   - USB Full/Ext  (vbus && !charging): Accent USB plug icon + Accent outline + Good fill
 *   - Battery Mode  (!vbus):             Clean battery icon, Good (or Alert <=20%) */
static uint16_t batch_add_battery_widget(rect_batch_t *b, uint8_t y0,
                                         pixel_hud_live_t const *l, hud_theme_t const *th)
{
  bool     usb_chg   = l->vbus_present && l->charging;
  bool     usb_full  = l->vbus_present && !l->charging;
  uint16_t batt_col  = (l->battery_pct <= 20U && !usb_chg) ? th->alert
                     : (usb_chg ? th->warn : th->good);
  uint16_t frame_col = usb_chg ? th->warn : (usb_full ? th->accent : th->muted);

  if (usb_chg) {
    batch_add(b, 69U, (uint8_t)(y0 + 0U), 3U, 2U, th->warn);
    batch_add(b, 67U, (uint8_t)(y0 + 2U), 4U, 2U, th->text);
    batch_add(b, 66U, (uint8_t)(y0 + 4U), 6U, 2U, th->warn);
    batch_add(b, 68U, (uint8_t)(y0 + 6U), 4U, 2U, th->text);
    batch_add(b, 67U, (uint8_t)(y0 + 8U), 3U, 2U, th->warn);
  } else if (usb_full) {
    batch_add(b, 64U, (uint8_t)(y0 + 4U), 2U, 2U, th->accent);
    batch_add(b, 66U, (uint8_t)(y0 + 2U), 4U, 6U, th->accent);
    batch_add(b, 70U, (uint8_t)(y0 + 3U), 3U, 1U, th->text);
    batch_add(b, 70U, (uint8_t)(y0 + 6U), 3U, 1U, th->text);
    batch_add(b, 67U, (uint8_t)(y0 + 4U), 2U, 2U, th->text);
  }

  batch_add(b, 75U, y0, 15U, 1U, frame_col);
  batch_add(b, 75U, (uint8_t)(y0 + 9U), 15U, 1U, frame_col);
  batch_add(b, 75U, y0, 1U, 10U, frame_col);
  batch_add(b, 89U, y0, 1U, 10U, frame_col);
  batch_add(b, 90U, (uint8_t)(y0 + 3U), 2U, 4U, frame_col);
  batch_add(b, 77U, (uint8_t)(y0 + 2U), 11U, 6U, th->dim);
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
                   (c->win_lock ? 32U : 0U) |
                   ((c->sleep_timeout_min > 0U) ? 64U : 0U));
}

/* Draw top status bar (y = 0..19) styled per active theme */
static void draw_home_header(pixel_hud_t *hud)
{
  pixel_hud_live_t const *l  = &hud->live;
  hud_theme_t const      *th = active_theme(hud);
  uint16_t hbg = th->header_bg;

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 0U, 0U, 128U, 18U, hbg);
  if (th->id == TFT_THEME_STOCK_PLUS) {
    batch_add(&b, 0U, 17U, 128U, 2U, th->accent); /* Classic GamaKay top blue ribbon rule */
  } else if (th->id == TFT_THEME_TERMINAL) {
    batch_add(&b, 0U, 17U, 128U, 1U, th->accent);
    batch_add(&b, 0U, 18U, 128U, 1U, th->bg);
    batch_add(&b, 0U, 19U, 128U, 1U, th->border);
  } else {
    batch_add(&b, 0U, 18U, 128U, 1U, th->border);
  }
  uint16_t batt_col = batch_add_battery_widget(&b, 4U, l, th);
  batch_submit(hud, &b, false);

  if (l->macro_recording) {
    char rec_str[7] = { 'R', 'E', 'C', ' ', 'M', (char)('0' + (l->macro_rec_slot % 10U)), '\0' };
    q_text(hud, 4U, 3U, rec_str, th->alert, hbg, 1U, false, false);
  } else if (l->macro_speed_toast != 0U) {
    q_text(hud, 4U, 3U, macro_player_speed_label(l->macro_speed_toast), th->warn, hbg, 1U, false, false);
  } else {
    char mode_str[5];
    uint16_t mode_col = th->accent;
    uint8_t  x_off    = 0U;
    if (th->id == TFT_THEME_TERMINAL) {
      mode_str[0] = '>';
      x_off       = 1U;
    }
    if (l->conn_mode == 1U) {
      mode_str[x_off + 0U] = '2'; mode_str[x_off + 1U] = '.'; mode_str[x_off + 2U] = '4';
      mode_col = th->good;
    } else if (l->conn_mode == 2U) {
      mode_str[x_off + 0U] = 'B'; mode_str[x_off + 1U] = 'T';
      mode_str[x_off + 2U] = (char)('1' + (l->bt_slot % 3U));
      mode_col = th->warn;
    } else {
      mode_str[x_off + 0U] = 'U'; mode_str[x_off + 1U] = 'S'; mode_str[x_off + 2U] = 'B';
      mode_col = th->accent;
    }
    mode_str[x_off + 3U] = '\0';
    q_text(hud, 4U, 3U, mode_str, mode_col, hbg, 1U, false, false);

    char lay_str[3] = { 'L', (char)('0' + (l->active_layer & 3U)), '\0' };
    q_text(hud, (uint8_t)(28U + (x_off * 6U)), 3U, lay_str, th->text, hbg, 1U, false, false);
  }

  char bstr[5];
  fmt_u8_pct(l->battery_pct, bstr);
  q_text(hud, 95U, 3U, bstr, batt_col, hbg, 1U, false, false);

  hud->prev_header_hash = compute_header_hash(hud);
}

/* Draw pixel-art animated Hall-effect switch inside left card (PIXEL & TERMINAL themes) */
static void draw_switch_animation(pixel_hud_t *hud, uint8_t dy, bool actuated, bool flush)
{
  hud_theme_t const *th = active_theme(hud);
  if (dy > PLUNGER_MAX_PX) {
    dy = PLUNGER_MAX_PX;
  }
  uint8_t act_px = (uint8_t)(((uint32_t)hud->cfg->hall_actuation_um * PLUNGER_MAX_PX) / 4000U);
  if (act_px < 1U) act_px = 1U;
  if (act_px > PLUNGER_MAX_PX) act_px = PLUNGER_MAX_PX;

  uint16_t stem_col   = actuated ? th->good : ((dy > 0U) ? th->accent : th->keytop);
  uint16_t sensor_col = actuated ? th->good : ((dy > 2U) ? th->accent : th->border);
  uint16_t cap_top    = actuated ? th->good : ((dy > 0U) ? th->accent : th->keytop);
  uint16_t interior   = (th->id == TFT_THEME_TERMINAL) ? th->bg : th->card;

  rect_batch_t b;
  batch_init(&b);

  batch_add(&b, 6U, 25U, 50U, 80U, interior);
  batch_add(&b, 11U, 56U, 40U, 45U, th->housing);
  batch_add(&b, 15U, 59U, 32U, 38U, th->dim);

  uint8_t act_y = (uint8_t)(60U + act_px);
  batch_add(&b, 15U, act_y, 32U, 1U, actuated ? th->good : th->warn);

  uint8_t spring_top = (uint8_t)(60U + dy);
  uint8_t spring_bot = 89U;
  if (spring_bot > spring_top + 4U) {
    uint8_t span = (uint8_t)(spring_bot - spring_top);
    batch_add(&b, 24U, (uint8_t)(spring_top + (span / 4U)), 14U, 2U, th->spring);
    batch_add(&b, 22U, (uint8_t)(spring_top + (span / 2U)), 18U, 2U, th->spring);
    batch_add(&b, 24U, (uint8_t)(spring_top + ((span * 3U) / 4U)), 14U, 2U, th->spring);
  } else {
    batch_add(&b, 22U, 86U, 18U, 3U, th->spring);
  }

  batch_add(&b, 21U, 91U, 20U, 5U, sensor_col);
  batch_add(&b, 13U, (uint8_t)(27U + dy), 36U, 4U, cap_top);
  batch_add(&b, 10U, (uint8_t)(31U + dy), 42U, 14U, th->keycap);
  batch_add(&b, 14U, (uint8_t)(34U + dy), 6U, 2U, th->text);
  batch_add(&b, 25U, (uint8_t)(45U + dy), 12U, 15U, stem_col);
  batch_add(&b, 28U, (uint8_t)(57U + dy), 6U, 3U, th->text);

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

  if (hud->smooth_travel_um == 0U) {
    hud->smooth_travel_um = raw;
  } else {
    hud->smooth_travel_um = (uint16_t)(((uint32_t)hud->smooth_travel_um + (uint32_t)raw + 1U) >> 1U);
  }
  return hud->smooth_travel_um;
}

static void get_display_hms(pixel_hud_t const *hud, uint8_t *h, uint8_t *m, uint8_t *s)
{
  pixel_hud_live_t const   *l = &hud->live;
  tk75_persist_cfg_t const *c = hud->cfg;
  if (l->rtc_valid) {
    uint8_t hh = l->rtc_hour;
    if (c->tft_clock_24h == 0U) {
      hh = (uint8_t)(hh % 12U);
      if (hh == 0U) hh = 12U;
    }
    *h = hh;
    *m = l->rtc_min;
    *s = l->rtc_sec;
  } else {
    uint32_t up = l->uptime_sec;
    *h = (uint8_t)((up / 3600U) % 100U);
    *m = (uint8_t)((up / 60U) % 60U);
    *s = (uint8_t)(up % 60U);
  }
}

/* Draw right-hand telemetry column on PIXEL & TERMINAL Home pages */
static void draw_home_right_stats(pixel_hud_t *hud, bool force_all, bool flush)
{
  hud_theme_t const *th = active_theme(hud);
  uint16_t wpm = hud->live.wpm;
  uint16_t disp_travel = hud->travel_active ? hud->smooth_travel_um : 0U;
  uint16_t travel_t = (disp_travel > 0U)
                        ? (uint16_t)((disp_travel + 50U) / 100U)
                        : (uint16_t)((hud->cfg->hall_actuation_um + 50U) / 100U);

  if (force_all || wpm != hud->prev_wpm) {
    char wbuf[4];
    fmt_wpm3(wpm, wbuf);
    q_text(hud, 64U, 26U, wbuf, th->text, th->bg, 2U, false, false);
    if (force_all) {
      q_text(hud, 104U, 38U, "WPM", th->muted, th->bg, 1U, false, false);
    }
    hud->prev_wpm = wpm;
  }

  if (force_all || travel_t != hud->prev_travel_disp || hud->live.any_actuated != hud->prev_actuated) {
    char mm[6];
    fmt_mm_tenths(travel_t, mm);
    uint16_t tcol = hud->live.any_actuated ? th->good
                  : ((disp_travel > 0U) ? th->accent : th->muted);
    q_text(hud, 64U, 58U, mm, tcol, th->bg, 1U, false, false);

    rect_batch_t b;
    batch_init(&b);
    batch_add(&b, 64U, 71U, 59U, 10U, th->bg);
    if (th->id == TFT_THEME_TERMINAL) {
      /* 8 retro CRT phosphor block segments across x=64..120 */
      uint8_t lit = (uint8_t)(((uint32_t)disp_travel * 8U + 2000U) / 4000U);
      if (lit > 8U) lit = 8U;
      for (uint8_t s = 0U; s < 8U; s++) {
        uint8_t sx = (uint8_t)(64U + (s * 7U));
        batch_add(&b, sx, 73U, 5U, 6U, (s < lit) ? tcol : th->dim);
      }
    } else {
      batch_add(&b, 64U, 73U, 58U, 6U, th->dim);
      uint8_t fill_w = (uint8_t)(((uint32_t)disp_travel * 58U) / 4000U);
      if (fill_w > 58U) fill_w = 58U;
      if (fill_w > 0U) {
        batch_add(&b, 64U, 73U, fill_w, 6U, tcol);
      }
    }
    uint8_t act_x = (uint8_t)(64U + (((uint32_t)hud->cfg->hall_actuation_um * 58U) / 4000U));
    if (act_x > 121U) act_x = 121U;
    batch_add(&b, act_x, 71U, 2U, 10U, th->warn);
    batch_submit(hud, &b, false);
    hud->prev_travel_disp = travel_t;
  }

  uint8_t bhash = compute_badges_hash(hud);
  if (force_all || bhash != hud->prev_badges_hash) {
    bool rt_on   = (hud->cfg->hall_rt_enabled != 0U);
    bool caps_on = ((hud->live.host_leds & 2U) != 0U);
    rect_batch_t b;
    batch_init(&b);
    if (th->id == TFT_THEME_TERMINAL) {
      batch_add(&b, 64U, 88U, 26U, 16U, rt_on ? th->warn : th->border);
      batch_add(&b, 65U, 89U, 24U, 14U, rt_on ? th->warn : th->bg);
      batch_add(&b, 94U, 88U, 28U, 16U, caps_on ? th->alert : th->border);
      batch_add(&b, 95U, 89U, 26U, 14U, caps_on ? th->alert : th->bg);
    } else {
      batch_add(&b, 64U, 88U, 26U, 16U, rt_on ? th->warn : th->card);
      batch_add(&b, 94U, 88U, 28U, 16U, caps_on ? th->alert : th->card);
    }
    batch_submit(hud, &b, false);

    uint16_t b1_bg = rt_on ? th->warn : ((th->id == TFT_THEME_TERMINAL) ? th->bg : th->card);
    uint16_t b2_bg = caps_on ? th->alert : ((th->id == TFT_THEME_TERMINAL) ? th->bg : th->card);
    q_text(hud, 71U, 90U, "RT", rt_on ? th->cursor_fg : th->muted, b1_bg, 1U, true, false);
    char const *b2 = caps_on ? "CAP" : (hud->cfg->mac_mode ? "MAC" : "WIN");
    q_text(hud, 99U, 90U, b2, caps_on ? th->text : th->accent, b2_bg, 1U, true, flush);
    hud->prev_badges_hash = bhash;
  } else if (flush) {
    uint8_t f = 0xFFU;
    (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, &f, 1U);
  }
}

/* STOCK+ Home dynamic updater: Classic GamaKay hero clock card + live Hall bar & WPM strip */
static void draw_stock_home_dynamic(pixel_hud_t *hud, uint8_t dy, bool actuated, bool force_all, bool flush)
{
  hud_theme_t const *th = active_theme(hud);
  uint8_t hh = 0U, mm = 0U, ss = 0U;
  get_display_hms(hud, &hh, &mm, &ss);
  uint8_t sec_odd = ss & 1U;

  uint16_t disp_travel = hud->travel_active ? hud->smooth_travel_um : 0U;
  uint16_t travel_t = (disp_travel > 0U)
                        ? (uint16_t)((disp_travel + 50U) / 100U)
                        : (uint16_t)((hud->cfg->hall_actuation_um + 50U) / 100U);

  bool clk_chg   = force_all || (hh != hud->prev_home_hour) || (mm != hud->prev_home_min) ||
                   (hud->live.rtc_valid != hud->prev_home_rtc_valid);
  bool col_chg   = force_all || (sec_odd != hud->prev_home_sec_odd);
  bool wpm_chg   = force_all || (hud->live.wpm != hud->prev_wpm);
  bool trv_chg   = force_all || (travel_t != hud->prev_travel_disp) ||
                   (dy != hud->anim_plunger_px) || (actuated != hud->prev_actuated);
  uint8_t bhash  = compute_badges_hash(hud);
  bool bdg_chg   = force_all || (bhash != hud->prev_badges_hash);

  /* Hero Clock Card (x=5..122, y=23..60): classic GamaKay big clock on left + WPM on right */
  if (clk_chg) {
    char h2[3], m2[3];
    fmt_u8_zero2(hh, h2);
    fmt_u8_zero2(mm, m2);
    q_text(hud, 10U, 29U, h2, th->text, th->card, 2U, false, false);
    q_text(hud, 46U, 29U, m2, th->text, th->card, 2U, false, false);
    hud->prev_home_hour      = hh;
    hud->prev_home_min       = mm;
    hud->prev_home_rtc_valid = hud->live.rtc_valid;
  }
  if (col_chg) {
    q_text(hud, 34U, 29U, ":", (sec_odd == 0U) ? th->warn : th->muted, th->card, 2U, false, false);
    hud->prev_home_sec_odd = sec_odd;
  }
  if (wpm_chg) {
    char w3[4];
    fmt_wpm3(hud->live.wpm, w3);
    q_text(hud, 82U, 28U, w3, th->accent, th->card, 1U, false, false);
    if (force_all) {
      q_text(hud, 102U, 28U, "WPM", th->muted, th->card, 1U, false, false);
    }
    hud->prev_wpm = hud->live.wpm;
  }

  if (trv_chg) {
    char mm_s[6];
    fmt_mm_tenths(travel_t, mm_s);
    uint16_t tcol = actuated ? th->good : ((disp_travel > 0U) ? th->accent : th->muted);
    q_text(hud, 82U, 42U, mm_s, tcol, th->card, 1U, false, false);

    /* Full-width 10-block stepped Hall travel gauge inside bottom card (x=8..119, y=67..84) */
    rect_batch_t b;
    batch_init(&b);
    batch_add(&b, 8U, 69U, 112U, 12U, th->card);
    uint8_t segs = (uint8_t)(((uint32_t)disp_travel * 10U + 2000U) / 4000U);
    if (segs > 10U) segs = 10U;
    for (uint8_t s = 0U; s < 10U; s++) {
      uint8_t sx = (uint8_t)(9U + (s * 11U));
      batch_add(&b, sx, 71U, 9U, 8U, (s < segs) ? tcol : th->dim);
    }
    uint8_t act_x = (uint8_t)(9U + (((uint32_t)hud->cfg->hall_actuation_um * 108U) / 4000U));
    if (act_x > 117U) act_x = 117U;
    batch_add(&b, act_x, 69U, 2U, 12U, th->warn);
    batch_submit(hud, &b, false);

    hud->anim_plunger_px  = dy;
    hud->prev_actuated    = actuated;
    hud->prev_travel_disp = travel_t;
  }

  if (bdg_chg) {
    bool rt_on   = (hud->cfg->hall_rt_enabled != 0U);
    bool caps_on = ((hud->live.host_leds & 2U) != 0U);
    bool eco_on  = (hud->cfg->sleep_timeout_min > 0U);
    rect_batch_t b;
    batch_init(&b);
    batch_add(&b, 8U, 87U, 30U, 16U, rt_on ? th->warn : th->dim);
    batch_add(&b, 44U, 87U, 36U, 16U, caps_on ? th->alert : th->dim);
    batch_add(&b, 86U, 87U, 34U, 16U, eco_on ? th->accent : th->dim);
    batch_submit(hud, &b, false);

    q_text(hud, 17U, 89U, "RT", rt_on ? th->cursor_fg : th->muted, rt_on ? th->warn : th->dim, 1U, true, false);
    char const *b2 = caps_on ? "CAPS" : (hud->cfg->mac_mode ? "MAC" : "WIN");
    q_text(hud, caps_on ? 50U : 53U, 89U, b2, caps_on ? th->text : th->accent, caps_on ? th->alert : th->dim, 1U, true, false);
    q_text(hud, 94U, 89U, "ECO", eco_on ? th->cursor_fg : th->muted, eco_on ? th->accent : th->dim, 1U, true, flush);
    hud->prev_badges_hash = bhash;
  } else if (flush) {
    uint8_t f = 0xFFU;
    (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, &f, 1U);
  }
}

/* Bottom page-indicator strip (y = 111..127) styled per active theme */
static void draw_footer(pixel_hud_t *hud, bool flush)
{
  hud_theme_t const *th = active_theme(hud);
  rect_batch_t b;
  batch_init(&b);

  if (th->id == TFT_THEME_STOCK_PLUS) {
    batch_add(&b, 0U, 111U, 128U, 1U, th->accent);
    batch_add(&b, 0U, 112U, 128U, 16U, th->header_bg);
    /* Classic Stock segmented page tabs (7 x 7px tabs) */
    uint8_t x = 5U;
    for (uint8_t i = 0U; i < PIXEL_PAGE_COUNT; i++) {
      bool cur = (i == hud->page);
      batch_add(&b, x, cur ? 116U : 119U, 7U, cur ? 6U : 3U, cur ? th->warn : th->muted);
      x = (uint8_t)(x + 9U);
    }
    batch_submit(hud, &b, false);
    char const *hint = hud->menu_open ? (hud->editing ? "DIAL:SET" : "PUSH:OK ") : "FN+KNOB ";
    uint16_t hcol    = hud->editing ? th->warn : (hud->menu_open ? th->text : th->muted);
    q_text(hud, 74U, 114U, hint, hcol, th->header_bg, 1U, false, flush);
    return;
  }

  if (th->id == TFT_THEME_TERMINAL) {
    batch_add(&b, 0U, 110U, 128U, 1U, th->border);
    batch_add(&b, 0U, 111U, 128U, 1U, th->accent);
    batch_add(&b, 0U, 112U, 128U, 16U, th->card);
    uint8_t x = 5U;
    for (uint8_t i = 0U; i < PIXEL_PAGE_COUNT; i++) {
      bool cur = (i == hud->page);
      batch_add(&b, x, 117U, cur ? 8U : 4U, 5U, cur ? th->accent : th->border);
      x = (uint8_t)(x + (cur ? 10U : 7U));
    }
    batch_submit(hud, &b, false);
    char const *hint = hud->menu_open ? (hud->editing ? "SET:TURN" : "OK:EDIT ") : "FN+KNOB ";
    uint16_t hcol    = hud->editing ? th->warn : (hud->menu_open ? th->accent : th->muted);
    q_text(hud, 74U, 114U, hint, hcol, th->card, 1U, false, flush);
    return;
  }

  /* PIXEL theme */
  batch_add(&b, 0U, 111U, 128U, 1U, th->border);
  batch_add(&b, 0U, 112U, 128U, 16U, th->card);
  uint8_t x = 5U;
  for (uint8_t i = 0U; i < PIXEL_PAGE_COUNT; i++) {
    if (i == hud->page) {
      batch_add(&b, x, 118U, 9U, 4U, th->accent);
      x = (uint8_t)(x + 12U);
    } else {
      batch_add(&b, x, 118U, 4U, 4U, th->muted);
      x = (uint8_t)(x + 7U);
    }
  }
  batch_submit(hud, &b, false);

  char const *hint = hud->menu_open ? (hud->editing ? "TURN:SET" : "OK:EDIT ") : "FN+KNOB ";
  uint16_t hcol    = hud->editing ? th->warn : (hud->menu_open ? th->accent : th->muted);
  q_text(hud, 74U, 114U, hint, hcol, th->card, 1U, false, flush);
}

/* Draw reusable menu top tab bar for pages 1..6 (y = 0..19) */
static void draw_menu_header(pixel_hud_t *hud, char const *title, uint8_t icon_kind)
{
  hud_theme_t const *th = active_theme(hud);
  bool tab_focused = (hud->focus_row == 0U);
  uint16_t hbg = tab_focused ? th->header_focus : th->header_bg;

  if (th->id == TFT_THEME_TERMINAL) {
    rect_batch_t b;
    batch_init(&b);
    batch_add(&b, 0U, 0U, 128U, 17U, hbg);
    batch_add(&b, 0U, 17U, 128U, 1U, tab_focused ? th->warn : th->accent);
    batch_add(&b, 0U, 18U, 128U, 1U, th->bg);
    batch_add(&b, 0U, 19U, 128U, 1U, th->border);
    batch_submit(hud, &b, false);

    char prompt[16];
    prompt[0] = '>';
    size_t n = strlen(title);
    if (n > 12U) n = 12U;
    memcpy(&prompt[1], title, n);
    prompt[1U + n] = '\0';
    q_text(hud, 4U, 3U, prompt, tab_focused ? th->warn : th->accent, hbg, 1U, false, false);

    char idx_s[6] = { '[', (char)('0' + hud->page), '/', (char)('0' + (PIXEL_PAGE_COUNT - 1U)), ']', '\0' };
    q_text(hud, 94U, 3U, idx_s, tab_focused ? th->text : th->muted, hbg, 1U, false, false);
    return;
  }

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 0U, 0U, 128U, 18U, hbg);
  batch_add(&b, 0U, 18U, 128U, 1U, tab_focused ? th->warn : ((th->id == TFT_THEME_STOCK_PLUS) ? th->accent : th->border));

  /* Minimal 8x8 pixel-art page icon at (6, 5) */
  if (icon_kind == 0U) {
    /* Sun (Brightness) */
    batch_add(&b, 8U, 7U, 4U, 4U, th->warn);
    batch_add(&b, 9U, 4U, 2U, 2U, th->warn);
    batch_add(&b, 9U, 12U, 2U, 2U, th->warn);
    batch_add(&b, 5U, 8U, 2U, 2U, th->warn);
    batch_add(&b, 13U, 8U, 2U, 2U, th->warn);
  } else if (icon_kind == 1U) {
    /* Switch stem (Hall Effect) */
    batch_add(&b, 6U, 5U, 8U, 3U, th->accent);
    batch_add(&b, 8U, 8U, 4U, 5U, th->good);
  } else if (icon_kind == 2U) {
    /* Play triangle (Macro) */
    batch_add(&b, 7U, 5U, 2U, 8U, th->good);
    batch_add(&b, 9U, 6U, 2U, 6U, th->good);
    batch_add(&b, 11U, 7U, 2U, 4U, th->warn);
    batch_add(&b, 13U, 8U, 2U, 2U, th->warn);
  } else if (icon_kind == 4U) {
    /* RGB Palette (Lighting) */
    batch_add(&b, 6U, 5U, 4U, 4U, th->alert);
    batch_add(&b, 11U, 5U, 4U, 4U, th->good);
    batch_add(&b, 8U, 10U, 4U, 4U, th->accent);
  } else if (icon_kind == 5U) {
    /* Lightning / Eco Power bolt */
    batch_add(&b, 9U, 4U, 3U, 2U, th->good);
    batch_add(&b, 7U, 6U, 4U, 2U, th->text);
    batch_add(&b, 6U, 8U, 6U, 2U, th->warn);
    batch_add(&b, 8U, 10U, 4U, 2U, th->text);
    batch_add(&b, 7U, 12U, 3U, 2U, th->good);
  } else {
    /* Gear (System) */
    batch_add(&b, 7U, 6U, 6U, 6U, tab_focused ? th->text : th->accent);
    batch_add(&b, 9U, 8U, 2U, 2U, hbg);
  }
  batch_submit(hud, &b, false);

  uint16_t title_fg = tab_focused ? ((th->id == TFT_THEME_STOCK_PLUS) ? th->text : th->accent) : th->text;
  q_text(hud, 20U, 3U, title, title_fg, hbg, 1U, false, false);
  q_text(hud, 108U, 3U, "<>", tab_focused ? th->warn : th->muted, hbg, 1U, false, false);
}

/* Shared themed row frame builder (supports compact h=19 & tall h=24 rows across all 3 skins) */
static void batch_add_row_frame(rect_batch_t *b, hud_theme_t const *th, uint8_t y, uint8_t h,
                                bool focused, bool editing, uint16_t *out_bg)
{
  uint16_t edge = editing ? th->warn : (focused ? th->accent : th->border);
  uint16_t bg   = focused ? th->focus_bg : th->bg;
  *out_bg       = bg;

  if (th->id == TFT_THEME_STOCK_PLUS) {
    /* Stock+ classic full-width row strip with bright accent bar & bottom hairline */
    batch_add(b, 2U, y, 124U, h, focused ? bg : th->card);
    if (focused) {
      batch_add(b, 2U, y, 124U, 1U, edge);
      batch_add(b, 2U, (uint8_t)(y + h - 1U), 124U, 1U, edge);
      batch_add(b, 2U, y, 4U, h, editing ? th->warn : th->accent);
    } else {
      batch_add(b, 4U, (uint8_t)(y + h - 1U), 120U, 1U, th->border);
    }
    *out_bg = focused ? bg : th->card;
    return;
  }

  if (th->id == TFT_THEME_TERMINAL) {
    /* Terminal CRT open corner-bracketed row */
    batch_add(b, 4U, y, 120U, h, bg);
    if (focused) {
      batch_add(b, 4U, y, 6U, 1U, edge);
      batch_add(b, 4U, y, 1U, h, edge);
      batch_add(b, 4U, (uint8_t)(y + h - 1U), 6U, 1U, edge);
      batch_add(b, 118U, y, 6U, 1U, edge);
      batch_add(b, 123U, y, 1U, h, edge);
      batch_add(b, 118U, (uint8_t)(y + h - 1U), 6U, 1U, edge);
    } else {
      batch_add(b, 4U, (uint8_t)(y + h - 1U), 120U, 1U, th->dim);
    }
    return;
  }

  /* PIXEL theme */
  batch_add(b, 4U, y, 120U, h, edge);
  batch_add(b, 5U, (uint8_t)(y + 1U), 118U, (uint8_t)(h - 2U), bg);
  if (focused) {
    batch_add(b, 5U, (uint8_t)(y + 1U), 3U, (uint8_t)(h - 2U), editing ? th->warn : th->accent);
  }
}

/* Draw one compact slider row (height 19 px) styled per active theme */
static void draw_slider_row_compact(pixel_hud_t *hud, uint8_t row_idx, uint8_t y,
                                    char const *label, uint8_t pct, uint16_t bar_col)
{
  hud_theme_t const *th = active_theme(hud);
  bool focused = (hud->focus_row == row_idx);
  bool editing = focused && hud->editing;
  uint16_t bg  = th->bg;

  rect_batch_t b;
  batch_init(&b);
  batch_add_row_frame(&b, th, y, 19U, focused, editing, &bg);
  batch_submit(hud, &b, false);

  /* Track & segments in their own clean batch (<= 11 rects, guaranteed under MAX_BATCH_RECTS) */
  batch_init(&b);
  uint16_t fill_col = editing ? th->warn : bar_col;
  if (th->id == TFT_THEME_STOCK_PLUS) {
    /* Stock+ 10-step segmented progress bar @ x=38..91 */
    uint8_t lit = (uint8_t)((pct + 5U) / 10U);
    if (lit > 10U) lit = 10U;
    for (uint8_t s = 0U; s < 10U; s++) {
      uint8_t sx = (uint8_t)(38U + (s * 5U));
      batch_add(&b, sx, (uint8_t)(y + 5U), 4U, 9U, (s < lit) ? fill_col : th->dim);
    }
  } else if (th->id == TFT_THEME_TERMINAL) {
    /* Terminal 8 chunky CRT blocks @ x=38..89 */
    uint8_t lit = (uint8_t)(((uint32_t)pct * 8U + 50U) / 100U);
    if (lit > 8U) lit = 8U;
    for (uint8_t s = 0U; s < 8U; s++) {
      uint8_t sx = (uint8_t)(38U + (s * 6U));
      batch_add(&b, sx, (uint8_t)(y + 6U), 4U, 7U, (s < lit) ? fill_col : th->dim);
    }
  } else {
    batch_add(&b, 36U, (uint8_t)(y + 6U), 54U, 7U, th->dim);
    uint8_t fill_w = (uint8_t)(((uint32_t)pct * 54U + 50U) / 100U);
    if (fill_w > 54U) fill_w = 54U;
    if (fill_w > 0U) {
      batch_add(&b, 36U, (uint8_t)(y + 6U), fill_w, 7U, fill_col);
    }
  }
  batch_submit(hud, &b, false);

  if (focused && (th->id == TFT_THEME_STOCK_PLUS || th->id == TFT_THEME_TERMINAL)) {
    char pfx[6];
    pfx[0] = editing ? '*' : '>';
    size_t n = strlen(label);
    if (n > 4U) n = 4U;
    memcpy(&pfx[1], label, n);
    pfx[1U + n] = '\0';
    q_text(hud, 8U, (uint8_t)(y + 4U), pfx, editing ? th->warn : th->text, bg, 1U, false, false);
  } else {
    q_text(hud, 11U, (uint8_t)(y + 4U), label, focused ? th->text : th->muted, bg, 1U, false, false);
  }

  char pstr[5];
  fmt_u8_pct(pct, pstr);
  q_text(hud, 95U, (uint8_t)(y + 4U), pstr, editing ? th->warn : th->text, bg, 1U, false, false);
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

/* Draw one key-value row (compact h=19 or chunky h=24) */
static void draw_kv_row(pixel_hud_t *hud, uint8_t row_idx, uint8_t y, uint8_t h,
                        char const *label, char const *val, uint16_t val_col)
{
  hud_theme_t const *th = active_theme(hud);
  bool focused = (hud->focus_row == row_idx);
  bool editing = focused && hud->editing;
  uint16_t bg  = th->bg;
  uint8_t ty   = (uint8_t)(y + ((h > 20U) ? 6U : 4U));

  rect_batch_t b;
  batch_init(&b);
  batch_add_row_frame(&b, th, y, h, focused, editing, &bg);
  batch_submit(hud, &b, false);

  if (focused && (th->id == TFT_THEME_STOCK_PLUS || th->id == TFT_THEME_TERMINAL)) {
    char pfx[7];
    pfx[0] = editing ? '*' : '>';
    size_t n = strlen(label);
    if (n > 5U) n = 5U;
    memcpy(&pfx[1], label, n);
    pfx[1U + n] = '\0';
    q_text(hud, 8U, ty, pfx, editing ? th->warn : th->text, bg, 1U, false, false);
  } else {
    q_text(hud, 12U, ty, label, focused ? th->text : th->muted, bg, 1U, false, false);
  }
  q_text(hud, 62U, ty, val, editing ? th->warn : val_col, bg, 1U, false, false);
}

static void draw_value_row_compact(pixel_hud_t *hud, uint8_t row_idx, uint8_t y,
                                   char const *label, char const *val, uint16_t val_col)
{
  draw_kv_row(hud, row_idx, y, 19U, label, val, val_col);
}

static void draw_value_row(pixel_hud_t *hud, uint8_t row_idx, uint8_t y,
                           char const *label, char const *val, uint16_t val_col)
{
  draw_kv_row(hud, row_idx, y, 24U, label, val, val_col);
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
    return COLOR_PRESET_COUNT - 1U;
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
  hud_theme_t const *th = active_theme(hud);
  bool focused = (hud->focus_row == row_idx);
  bool editing = focused && hud->editing;
  uint16_t bg  = th->bg;
  uint16_t swatch = hsv_to_rgb565(hue, sat);
  uint8_t  pidx   = find_nearest_color_preset(hue, sat);

  rect_batch_t b;
  batch_init(&b);
  batch_add_row_frame(&b, th, y, 19U, focused, editing, &bg);
  batch_add(&b, 101U, (uint8_t)(y + 3U), 18U, 13U, th->text);
  batch_add(&b, 102U, (uint8_t)(y + 4U), 16U, 11U, swatch);
  batch_submit(hud, &b, false);

  if (focused && (th->id == TFT_THEME_STOCK_PLUS || th->id == TFT_THEME_TERMINAL)) {
    char pfx[7];
    pfx[0] = editing ? '*' : '>';
    size_t n = strlen(label);
    if (n > 5U) n = 5U;
    memcpy(&pfx[1], label, n);
    pfx[1U + n] = '\0';
    q_text(hud, 8U, (uint8_t)(y + 4U), pfx, editing ? th->warn : th->text, bg, 1U, false, false);
  } else {
    q_text(hud, 12U, (uint8_t)(y + 4U), label, focused ? th->text : th->muted, bg, 1U, false, false);
  }
  q_text(hud, 62U, (uint8_t)(y + 4U), k_color_presets[pidx].name,
         editing ? th->warn : th->text, bg, 1U, false, false);
}

static void render_full_page(pixel_hud_t *hud)
{
  tk75_persist_cfg_t const *c  = hud->cfg;
  hud_theme_t const        *th = active_theme(hud);
  hud->applied_theme = th->id;

  if (hud->page == PIXEL_PAGE_HOME) {
    q_fill(hud, 0U, 19U, 128U, 92U, th->bg, false);
    draw_home_header(hud);

    if (th->id == TFT_THEME_STOCK_PLUS) {
      /* Stock+ Home: Hero Clock/WPM Card + Bottom Live Hall Gauge & Badges Card */
      rect_batch_t b;
      batch_init(&b);
      batch_add(&b, 4U, 23U, 120U, 38U, th->border);
      batch_add(&b, 5U, 24U, 118U, 36U, th->card);
      batch_add(&b, 5U, 24U, 118U, 2U, th->warn); /* Signature Stock orange top accent */
      batch_add(&b, 4U, 65U, 120U, 42U, th->border);
      batch_add(&b, 5U, 66U, 118U, 40U, th->card);
      batch_submit(hud, &b, false);

      draw_stock_home_dynamic(hud, hud->anim_plunger_px, hud->live.any_actuated, true, false);
      draw_footer(hud, true);
      return;
    }

    /* PIXEL & TERMINAL Home: Left switch card frame + Right live telemetry */
    rect_batch_t b;
    batch_init(&b);
    if (th->id == TFT_THEME_TERMINAL) {
      /* Wireframe CRT corner brackets around switch chamber */
      batch_add(&b, 4U, 23U, 54U, 1U, th->border);
      batch_add(&b, 4U, 106U, 54U, 1U, th->border);
      batch_add(&b, 4U, 23U, 1U, 84U, th->border);
      batch_add(&b, 57U, 23U, 1U, 84U, th->border);
      batch_add(&b, 4U, 23U, 8U, 2U, th->accent);
      batch_add(&b, 50U, 23U, 8U, 2U, th->accent);
      batch_add(&b, 4U, 105U, 8U, 2U, th->accent);
      batch_add(&b, 50U, 105U, 8U, 2U, th->accent);
    } else {
      batch_add(&b, 4U, 23U, 54U, 84U, th->border);
      batch_add(&b, 5U, 24U, 52U, 82U, th->card);
    }
    batch_submit(hud, &b, false);

    draw_switch_animation(hud, hud->anim_plunger_px, hud->live.any_actuated, false);
    draw_home_right_stats(hud, true, false);
    draw_footer(hud, true);
    return;
  }

  q_fill(hud, 0U, 19U, 128U, 92U, th->bg, false);

  if (hud->page == PIXEL_PAGE_BRIGHTNESS) {
    draw_menu_header(hud, "BRIGHTNESS", 0U);
    draw_slider_row_compact(hud, 1U, 22U, "TFT", c->tft_brightness, th->accent);
    draw_slider_row_compact(hud, 2U, 43U, "RGB", u8_to_pct10(c->rgb_brightness, c->rgb_enabled), th->good);
    draw_slider_row_compact(hud, 3U, 64U, "SID", u8_to_pct10(c->side_brightness, c->side_enabled), th->warn);
    draw_value_row_compact(hud, 4U, 85U, "CAP ",
                           caps_side_fx_short_name(c->caps_side_enabled, c->caps_side_effect),
                           (c->caps_side_enabled != 0U) ? th->good : th->muted);
    draw_footer(hud, true);
    return;
  }

  if (hud->page == PIXEL_PAGE_LIGHTING) {
    draw_menu_header(hud, "LIGHTING", 4U);
    draw_value_row_compact(hud, 1U, 22U, "FX  ",
                           rgb_fx_short_name((c->rgb_enabled != 0U) ? c->rgb_effect : 0U),
                           (c->rgb_enabled != 0U && c->rgb_effect != 0U) ? th->accent : th->muted);
    draw_color_row_compact(hud, 2U, 43U, "COL ", c->rgb_hue, c->rgb_sat);
    draw_value_row_compact(hud, 3U, 64U, "SFX ",
                           side_fx_short_name(c->side_enabled, c->side_effect),
                           (c->side_enabled != 0U && c->side_effect != 0U) ? th->warn : th->muted);
    draw_color_row_compact(hud, 4U, 85U, "SCOL", c->side_hue, c->side_sat);
    draw_footer(hud, true);
    return;
  }

  if (hud->page == PIXEL_PAGE_SWITCH) {
    draw_menu_header(hud, "HALL SWITCH", 1U);
    char act_str[6];
    fmt_mm_tenths((uint16_t)((c->hall_actuation_um + 50U) / 100U), act_str);
    draw_value_row_compact(hud, 1U, 22U, "ACT ", act_str, th->accent);

    char rt_str[8] = "OFF   ";
    if (c->hall_rt_enabled != 0U) {
      fmt_mm_tenths((uint16_t)((c->hall_rt_down_um + 50U) / 100U), rt_str);
    }
    draw_value_row_compact(hud, 2U, 43U, "RT  ", rt_str, c->hall_rt_enabled ? th->warn : th->muted);

    char const *socd_str = (c->hall_socd_mode == HALL_SOCD_LAST_PRIORITY)  ? "LAST  "
                         : (c->hall_socd_mode == HALL_SOCD_NEUTRAL)        ? "NEUT  "
                         : (c->hall_socd_mode == HALL_SOCD_FIRST_PRIORITY) ? "FIRST "
                                                                           : "OFF   ";
    draw_value_row_compact(hud, 3U, 64U, "SOCD", socd_str,
                           (c->hall_socd_mode != HALL_SOCD_OFF) ? th->good : th->muted);
    draw_value_row_compact(hud, 4U, 85U, "CAL ", "START ", th->warn);
    draw_footer(hud, true);
    return;
  }

  if (hud->page == PIXEL_PAGE_MACRO) {
    draw_menu_header(hud, "MACRO", 2U);
    draw_value_row(hud, 1U, 24U, "SPD ", macro_speed_name(c->macro_speed_idx), th->accent);
    draw_value_row(hud, 2U, 53U, "STEP", macro_speed_detail(c->macro_speed_idx), th->warn);
    char slots_str[9];
    bool any_used = false;
    if (hud->focus_row == 3U && hud->editing) {
      static char const k_keys[5] = { 'C', 'V', 'B', 'N', 'M' };
      uint8_t s    = hud->macro_sel_slot % 5U;
      bool    used = macro_recorder_slot_has_data(c->macros, s);
      slots_str[0] = k_keys[s];
      slots_str[1] = ':';
      memcpy(&slots_str[2], used ? "PLAY" : "----", 5U);
      draw_value_row(hud, 3U, 82U, "KEYS", slots_str, used ? th->good : th->muted);
    } else {
      format_macro_slots(c->macros, slots_str, &any_used);
      draw_value_row(hud, 3U, 82U, "KEYS", slots_str, any_used ? th->good : th->muted);
    }
    draw_footer(hud, true);
    return;
  }

  if (hud->page == PIXEL_PAGE_POWER) {
    draw_menu_header(hud, "POWER", 5U);
    char eco_str[7];
    fmt_eco_timeout(c->sleep_timeout_min, eco_str);
    draw_value_row_compact(hud, 1U, 22U, "ECO ", eco_str,
                           (c->sleep_timeout_min > 0U) ? th->good : th->muted);
    uint8_t smode = normalize_saver_mode(c->tft_screensaver);
    draw_value_row_compact(hud, 2U, 43U, "SAVR", saver_mode_short_name(smode),
                           (smode != (uint8_t)TFT_SAVER_OFF) ? th->accent : th->muted);
    draw_value_row_compact(hud, 3U, 64U, "RATE", rf_rate_short_name(c->rf_poll_rate_idx), th->warn);
    draw_value_row_compact(hud, 4U, 85U, "CLK ", c->tft_clock_24h ? "24H   " : "12H   ", th->text);
    draw_footer(hud, true);
    return;
  }

  /* PIXEL_PAGE_SYSTEM (4 compact rows fitting y=22..103 cleanly) */
  draw_menu_header(hud, "SYSTEM", 3U);
  draw_slider_row_compact(hud, 1U, 22U, "SPD", u8_to_pct10(c->rgb_speed, 1U), th->accent);
  draw_value_row_compact(hud, 2U, 43U, "OS  ", c->mac_mode ? "MACOS " : "WIN   ", th->good);
  draw_value_row_compact(hud, 3U, 64U, "WINL", c->win_lock ? "LOCKED" : "OFF   ",
                         c->win_lock ? th->alert : th->muted);
  draw_value_row_compact(hud, 4U, 85U, "SKIN", th->name, th->warn);
  draw_footer(hud, true);
}

/* Draw static chrome of fullscreen macro recording screen (128x128, themed) */
static void draw_macro_rec_static(pixel_hud_t *hud)
{
  hud_theme_t const *th = active_theme(hud);
  q_fill(hud, 0U, 0U, 128U, 128U, th->bg, false);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 2U, 2U, 12U, 2U, th->alert);
  batch_add(&b, 2U, 2U, 2U, 12U, th->alert);
  batch_add(&b, 114U, 2U, 12U, 2U, th->alert);
  batch_add(&b, 124U, 2U, 2U, 12U, th->alert);
  batch_add(&b, 2U, 124U, 12U, 2U, th->alert);
  batch_add(&b, 2U, 114U, 2U, 12U, th->alert);
  batch_add(&b, 114U, 124U, 12U, 2U, th->alert);
  batch_add(&b, 124U, 114U, 2U, 12U, th->alert);

  batch_add(&b, 12U, 28U, 104U, 46U, th->border);
  batch_add(&b, 14U, 30U, 100U, 42U, th->card);
  batch_add(&b, 18U, 34U, 92U, 26U, th->dim);
  batch_add(&b, 28U, 64U, 72U, 8U, th->housing);
  batch_add(&b, 36U, 66U, 6U, 4U, th->bg);
  batch_add(&b, 86U, 66U, 6U, 4U, th->bg);
  batch_submit(hud, &b, false);

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
  q_text(hud, 24U, 8U, hdr, th->text, th->bg, 1U, false, false);
  q_text(hud, 14U, 80U, "EVTS:", th->muted, th->bg, 1U, false, false);
  q_text(hud, 66U, 80U, "/341", th->muted, th->bg, 1U, false, false);
  q_text(hud, 14U, 109U, "FN:EDIT / SAVE", th->warn, th->bg, 1U, false, false);
}

static void draw_macro_rec_anim(pixel_hud_t *hud, bool force_text)
{
  hud_theme_t const *th = active_theme(hud);
  uint8_t  phase   = hud->rec_anim_phase & 3U;
  bool     dot_on  = (phase < 2U);
  uint16_t bytes   = hud->live.macro_rec_bytes;
  if (bytes > MACRO_REC_MAX_BYTES) {
    bytes = MACRO_REC_MAX_BYTES;
  }
  uint16_t evts = (uint16_t)(bytes / 3U);

  rect_batch_t b;
  batch_init(&b);
  uint16_t dot_col = dot_on ? th->alert : th->dim;
  batch_add(&b, 12U, 10U, 8U, 8U, dot_col);
  if (dot_on) {
    batch_add(&b, 14U, 12U, 2U, 2U, th->text);
  }
  batch_add(&b, 26U, 38U, 18U, 18U, th->dim);
  batch_add(&b, 84U, 38U, 18U, 18U, th->dim);
  batch_add(&b, 48U, 42U, 32U, 10U, th->bg);
  batch_add(&b, 28U, 40U, 14U, 14U, th->border);
  batch_add(&b, 30U, 42U, 10U, 10U, th->bg);
  batch_add(&b, 86U, 40U, 14U, 14U, th->border);
  batch_add(&b, 88U, 42U, 10U, 10U, th->bg);
  batch_submit(hud, &b, false);

  batch_init(&b);
  uint16_t spk = th->accent;
  if ((phase & 1U) == 0U) {
    batch_add(&b, 34U, 41U, 2U, 12U, spk);
    batch_add(&b, 29U, 46U, 12U, 2U, spk);
    batch_add(&b, 92U, 41U, 2U, 12U, spk);
    batch_add(&b, 87U, 46U, 12U, 2U, spk);
  } else {
    batch_add(&b, 31U, 43U, 3U, 3U, spk);
    batch_add(&b, 36U, 48U, 3U, 3U, spk);
    batch_add(&b, 36U, 43U, 3U, 3U, spk);
    batch_add(&b, 31U, 48U, 3U, 3U, spk);
    batch_add(&b, 89U, 43U, 3U, 3U, spk);
    batch_add(&b, 94U, 48U, 3U, 3U, spk);
    batch_add(&b, 94U, 43U, 3U, 3U, spk);
    batch_add(&b, 89U, 48U, 3U, 3U, spk);
  }
  batch_add(&b, 33U, 45U, 4U, 4U, th->text);
  batch_add(&b, 91U, 45U, 4U, 4U, th->text);

  uint8_t wave_x = (uint8_t)(50U + (phase * 6U));
  batch_add(&b, wave_x, 45U, 4U, 4U, th->alert);
  uint8_t wave_x2 = (uint8_t)(50U + (((phase + 2U) & 3U) * 6U));
  batch_add(&b, wave_x2, 46U, 4U, 2U, th->warn);
  batch_submit(hud, &b, false);

  batch_init(&b);
  batch_add(&b, 13U, 94U, 102U, 10U, th->border);
  batch_add(&b, 14U, 95U, 100U, 8U, th->dim);
  uint8_t fill_w = (uint8_t)(((uint32_t)bytes * 100U) / MACRO_REC_MAX_BYTES);
  if (fill_w > 100U) fill_w = 100U;
  if (fill_w > 0U) {
    uint16_t bar_col = (fill_w >= 80U) ? th->alert : ((fill_w >= 50U) ? th->warn : th->good);
    batch_add(&b, 14U, 95U, fill_w, 8U, bar_col);
  }
  bool text_chg = force_text || (bytes != hud->prev_rec_bytes);
  batch_submit(hud, &b, !text_chg);

  if (text_chg) {
    char cbuf[4];
    fmt_wpm3(evts, cbuf);
    q_text(hud, 46U, 80U, cbuf, th->text, th->bg, 1U, false, true);
    hud->prev_rec_bytes = bytes;
  }
}

/* ---- Fullscreen 128x128 Macro Text Editor (5 rows x 19 cols, German ISO, themed) ---- */

#define EDIT_VIEW_ROWS 5U
#define EDIT_VIEW_COLS MACRO_EDITOR_COLS

static void macro_tok_to_cell(hud_theme_t const *th, macro_edit_tok_t tok, char *out_ch, uint16_t *out_fg)
{
  bool shift  = ((tok.mods & 0x22U) != 0U);
  bool altgr  = ((tok.mods & 0x40U) != 0U);
  bool combo  = ((tok.mods & 0x9DU) != 0U);
  uint8_t hid = tok.hid;
  char ch     = '?';
  uint16_t fg = combo ? th->warn : th->text;

  if (hid >= (uint8_t)KC_A && hid <= (uint8_t)KC_Z) {
    char base = (char)('a' + (hid - (uint8_t)KC_A));
    if (hid == (uint8_t)KC_Y) base = 'z';
    else if (hid == (uint8_t)KC_Z) base = 'y';
    if (altgr && !combo && hid == (uint8_t)KC_Q) {
      ch = '@';
      fg = th->good;
    } else if (altgr && !combo && hid == (uint8_t)KC_E) {
      ch = 'E';
      fg = th->good;
    } else {
      ch = (shift && !combo) ? (char)(base - 'a' + 'A') : base;
    }
  } else if (hid >= (uint8_t)KC_1 && hid <= (uint8_t)KC_0) {
    static char const k_norm[10] = "1234567890";
    static char const k_shft[10] = "!\"$%&/()=";
    uint8_t idx = (uint8_t)(hid - (uint8_t)KC_1);
    if (altgr && !combo) {
      if (hid == (uint8_t)KC_7)      { ch = '{'; fg = th->good; }
      else if (hid == (uint8_t)KC_8) { ch = '['; fg = th->good; }
      else if (hid == (uint8_t)KC_9) { ch = ']'; fg = th->good; }
      else if (hid == (uint8_t)KC_0) { ch = '}'; fg = th->good; }
      else                           { ch = k_norm[idx]; }
    } else if (shift && !combo) {
      ch = (idx == 9U) ? '=' : k_shft[idx];
      if (idx == 2U) {
        fg = th->good;
      }
    } else {
      ch = k_norm[idx];
    }
  } else {
    switch (hid) {
      case KC_SPC:  ch = ' '; break;
      case KC_ENT:  ch = '>'; fg = combo ? th->warn : th->accent; break;
      case KC_TAB:  ch = 'T'; fg = combo ? th->warn : th->accent; break;
      case KC_BSPC: ch = '<'; fg = combo ? th->warn : th->alert; break;
      case KC_DEL:  ch = 'X'; fg = combo ? th->warn : th->alert; break;
      case KC_ESC:  ch = '!'; fg = th->alert; break;
      case KC_MINS: ch = altgr ? '\\' : (shift ? '?' : 's'); if (!shift && !altgr && !combo) fg = th->good; break;
      case KC_EQL:  ch = shift ? '`' : '\''; break;
      case KC_LBRC: ch = shift ? 'U' : 'u'; if (!combo) fg = th->good; break;
      case KC_RBRC: ch = altgr ? '~' : (shift ? '*' : '+'); break;
      case KC_SCLN: ch = shift ? 'O' : 'o'; if (!combo) fg = th->good; break;
      case KC_QUOT: ch = shift ? 'A' : 'a'; if (!combo) fg = th->good; break;
      case KC_GRV:  ch = '^'; break;
      case KC_NUHS: ch = shift ? '\'' : '#'; break;
      case KC_NUBS: ch = altgr ? '|' : (shift ? '>' : '<'); break;
      case KC_COMM: ch = shift ? ';' : ','; break;
      case KC_DOT:  ch = shift ? ':' : '.'; break;
      case KC_SLSH: ch = shift ? '_' : '-'; break;
      case KC_LEFT: ch = '<'; fg = th->accent; break;
      case KC_RGHT: ch = '>'; fg = th->accent; break;
      case KC_UP:   ch = '^'; fg = th->accent; break;
      case KC_DOWN: ch = 'v'; fg = th->accent; break;
      default:
        ch = (hid >= (uint8_t)KC_F1 && hid <= (uint8_t)KC_F12) ? 'F'
           : (hid >= 0xE0U && hid <= 0xE7U)                    ? 'M'
                                                               : '*';
        fg = combo ? th->warn : th->accent;
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

static void fmt_macro_tok_inspector(hud_theme_t const *th, pixel_hud_live_t const *l,
                                    char out[21], uint16_t *out_col)
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
    *out_col = th->accent;
    return;
  }

  macro_edit_tok_t tok = l->macro_edit_toks[cur];
  bool has_mod_prefix = false;
  if ((tok.mods & 0x11U) != 0U) { p = append_str(out, p, 20U, "CTL+"); has_mod_prefix = true; }
  if ((tok.mods & 0x04U) != 0U) { p = append_str(out, p, 20U, "ALT+"); has_mod_prefix = true; }
  if ((tok.mods & 0x88U) != 0U) { p = append_str(out, p, 20U, "WIN+"); has_mod_prefix = true; }
  if (has_mod_prefix && (tok.mods & 0x22U) != 0U) { p = append_str(out, p, 20U, "SHF+"); }

  char ch = '?';
  uint16_t fg = th->text;
  macro_tok_to_cell(th, tok, &ch, &fg);
  *out_col = has_mod_prefix ? th->warn : fg;

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
  hud_theme_t const *th = active_theme(hud);
  q_fill(hud, 0U, 0U, 128U, 128U, th->bg, false);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 0U, 0U, 128U, 18U, th->header_bg);
  batch_add(&b, 0U, 18U, 128U, 1U, th->accent);
  batch_add(&b, 2U, 20U, 124U, 74U, th->border);
  batch_add(&b, 3U, 21U, 122U, 72U, th->bg);
  batch_add(&b, 0U, 95U, 128U, 15U, th->card);
  batch_add(&b, 0U, 110U, 128U, 1U, th->border);
  batch_add(&b, 0U, 111U, 128U, 17U, th->header_bg);
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
  q_text(hud, 4U, 3U, hdr, th->accent, th->header_bg, 1U, false, false);
  q_text(hud, 4U, 113U, "FN:SAVE", th->good, th->header_bg, 1U, false, false);
  q_text(hud, 68U, 113U, "KNOB:QUIT", th->alert, th->header_bg, 1U, false, false);
}

static void draw_macro_edit_row(pixel_hud_t *hud, uint8_t vis_row, bool flush)
{
  hud_theme_t const      *th = active_theme(hud);
  pixel_hud_live_t const *l  = &hud->live;
  uint16_t base = (uint16_t)((uint16_t)(hud->edit_scroll_row + vis_row) * EDIT_VIEW_COLS);
  uint8_t  y    = (uint8_t)(22U + (vis_row * 14U));

  char     chars[EDIT_VIEW_COLS];
  uint16_t fgs[EDIT_VIEW_COLS];
  uint16_t bgs[EDIT_VIEW_COLS];

  for (uint8_t col = 0U; col < EDIT_VIEW_COLS; col++) {
    uint16_t idx = (uint16_t)(base + col);
    bool is_cur  = (idx == l->macro_edit_cursor);
    char ch      = ' ';
    uint16_t fg  = th->text;
    uint16_t bg  = th->bg;

    if (l->macro_edit_toks != NULL && idx < l->macro_edit_count) {
      macro_tok_to_cell(th, l->macro_edit_toks[idx], &ch, &fg);
    }
    if (is_cur) {
      bg = hud->edit_blink_on ? th->accent : th->warn;
      fg = th->cursor_fg;
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
  hud_theme_t const      *th = active_theme(hud);
  pixel_hud_live_t const *l  = &hud->live;
  uint16_t cnt = l->macro_edit_count;
  if (cnt > MACRO_EDITOR_MAX_TOKS) cnt = MACRO_EDITOR_MAX_TOKS;

  char cbuf[8];
  cbuf[0] = (cnt >= 100U) ? (char)('0' + ((cnt / 100U) % 10U)) : ' ';
  cbuf[1] = (cnt >= 10U)  ? (char)('0' + ((cnt / 10U) % 10U))  : ' ';
  cbuf[2] = (char)('0' + (cnt % 10U));
  cbuf[3] = '/'; cbuf[4] = '2'; cbuf[5] = '5'; cbuf[6] = '6'; cbuf[7] = '\0';
  uint16_t ccol = (cnt >= 240U) ? th->alert : ((cnt >= 192U) ? th->warn : th->text);
  q_text(hud, 82U, 3U, cbuf, ccol, th->header_bg, 1U, false, false);

  char insp[21];
  uint16_t icol = th->text;
  fmt_macro_tok_inspector(th, l, insp, &icol);
  q_text(hud, 4U, 96U, insp, icol, th->card, 1U, false, false);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 122U, 22U, 2U, 70U, th->dim);
  uint8_t total_rows = (uint8_t)((cnt / EDIT_VIEW_COLS) + 1U);
  if (total_rows > EDIT_VIEW_ROWS) {
    uint8_t thumb_h = (uint8_t)((70U * EDIT_VIEW_ROWS) / total_rows);
    if (thumb_h < 10U) thumb_h = 10U;
    uint8_t max_scroll = (uint8_t)(total_rows - EDIT_VIEW_ROWS);
    uint8_t thumb_y = (uint8_t)(22U + (((uint32_t)hud->edit_scroll_row * (70U - thumb_h)) / max_scroll));
    batch_add(&b, 122U, thumb_y, 2U, thumb_h, th->accent);
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

  while (hud->edit_dirty_mask != 0U && hud->q_count <= 24U) {
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
  h = (h ^ c->tft_theme) * 16777619U;
  return h;
}

static void draw_screensaver_static(pixel_hud_t *hud)
{
  hud_theme_t const *th = active_theme(hud);
  q_fill(hud, 0U, 0U, 128U, 128U, th->bg, false);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 0U, 22U, 128U, 1U, th->border);
  batch_add(&b, 4U, 25U, 120U, 43U, th->border);
  batch_add(&b, 5U, 26U, 118U, 41U, th->card);
  batch_add(&b, 4U, 72U, 58U, 36U, th->border);
  batch_add(&b, 5U, 73U, 56U, 34U, th->card);
  batch_add(&b, 66U, 72U, 58U, 36U, th->border);
  batch_add(&b, 67U, 73U, 56U, 34U, th->card);
  batch_add(&b, 0U, 111U, 128U, 1U, th->border);
  batch_add(&b, 0U, 112U, 128U, 16U, th->card);

  batch_add(&b, 8U, 4U, 5U, 2U, th->warn);
  batch_add(&b, 6U, 6U, 4U, 6U, th->warn);
  batch_add(&b, 8U, 12U, 5U, 2U, th->warn);
  batch_add(&b, 10U, 6U, 3U, 6U, th->bg);
  batch_submit(hud, &b, false);
}

static void draw_screensaver_sysinfo(pixel_hud_t *hud, bool flush)
{
  hud_theme_t const        *th = active_theme(hud);
  pixel_hud_live_t const   *l  = &hud->live;
  tk75_persist_cfg_t const *c  = hud->cfg;

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 63U, 3U, 30U, 15U, th->bg);
  uint16_t batt_col = batch_add_battery_widget(&b, 5U, l, th);
  batch_submit(hud, &b, false);

  q_text(hud, 20U, 5U, l->rtc_valid ? "CLK" : "UP ", l->rtc_valid ? th->accent : th->warn,
         th->bg, 1U, false, false);
  char bstr[5];
  fmt_u8_pct(l->battery_pct, bstr);
  q_text(hud, 95U, 4U, bstr, batt_col, th->bg, 1U, false, false);

  char tr_str[8] = "USB 8K";
  uint16_t tr_col = th->accent;
  if (l->conn_mode == 1U) {
    memcpy(tr_str, "2.4 1K", 7U);
    tr_col = th->good;
  } else if (l->conn_mode == 2U) {
    tr_str[0] = 'B'; tr_str[1] = 'T'; tr_str[2] = (char)('1' + (l->bt_slot % 3U));
    tr_str[3] = ' '; tr_str[4] = 'L'; tr_str[5] = 'E'; tr_str[6] = '\0';
    tr_col = th->warn;
  }
  q_text(hud, 9U, 76U, tr_str, tr_col, th->card, 1U, false, false);

  char os_str[7];
  if (c->mac_mode) {
    os_str[0] = 'M'; os_str[1] = 'A'; os_str[2] = 'C';
  } else {
    os_str[0] = 'W'; os_str[1] = 'I'; os_str[2] = 'N';
  }
  os_str[3] = ' '; os_str[4] = 'L'; os_str[5] = (char)('0' + (l->active_layer & 3U)); os_str[6] = '\0';
  q_text(hud, 9U, 92U, os_str, th->text, th->card, 1U, false, false);

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
  q_text(hud, 71U, 76U, act_str, c->hall_rt_enabled ? th->good : th->accent, th->card, 1U, false, false);

  char pk_str[8] = "PK    ";
  char w3[4];
  fmt_wpm3(l->peak_wpm, w3);
  pk_str[3] = w3[0]; pk_str[4] = w3[1]; pk_str[5] = w3[2]; pk_str[6] = '\0';
  q_text(hud, 71U, 92U, pk_str, th->warn, th->card, 1U, false, false);

  char left_b[10] = "TYPE:WAKE";
  uint16_t left_col = th->muted;
  if ((l->host_leds & 0x02U) != 0U) {
    memcpy(left_b, "CAPS ON  ", 10U);
    left_col = th->alert;
  } else if (c->win_lock != 0U) {
    memcpy(left_b, "WIN LOCK ", 10U);
    left_col = th->warn;
  } else if ((l->host_leds & 0x01U) != 0U) {
    memcpy(left_b, "NUM LOCK ", 10U);
    left_col = th->accent;
  } else if (c->hall_socd_mode != HALL_SOCD_OFF) {
    char const *sm = (c->hall_socd_mode == HALL_SOCD_LAST_PRIORITY)  ? "SOCD:LAST"
                   : (c->hall_socd_mode == HALL_SOCD_NEUTRAL)        ? "SOCD:NEUT"
                                                                     : "SOCD:1ST ";
    memcpy(left_b, sm, 10U);
    left_col = th->good;
  }
  q_text(hud, 5U, 114U, left_b, left_col, th->card, 1U, false, false);

  char kbuf[8];
  fmt_keys_compact(l->session_keys, kbuf);
  q_text(hud, 82U, 114U, kbuf, th->text, th->card, 1U, false, flush);

  hud->prev_saver_hash = compute_saver_hash(hud);
}

static void draw_screensaver_anim(pixel_hud_t *hud, bool force_all)
{
  hud_theme_t const      *th = active_theme(hud);
  pixel_hud_live_t const *l  = &hud->live;
  uint8_t phase = hud->saver_phase & 3U;

  uint8_t disp_h = 0U, disp_m = 0U, disp_s = 0U;
  get_display_hms(hud, &disp_h, &disp_m, &disp_s);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 40U, 3U, 22U, 17U, th->bg);
  batch_add(&b, 42U, 6U, 2U, 2U, (phase == 0U) ? th->text : th->dim);
  batch_add(&b, 50U, 13U, 2U, 2U, (phase == 1U) ? th->accent : th->border);
  batch_add(&b, 57U, 7U, 2U, 2U, (phase == 2U) ? th->warn : th->dim);
  batch_add(&b, 45U, 16U, 1U, 1U, (phase == 3U) ? th->text : th->border);
  batch_add(&b, 59U, 15U, 2U, 2U, ((phase & 1U) == 0U) ? th->accent : th->dim);

  batch_add(&b, 14U, 58U, 100U, 6U, th->card);
  batch_add(&b, 14U, 59U, 100U, 4U, th->dim);
  uint8_t sec_w = (uint8_t)(((uint32_t)disp_s * 100U) / 59U);
  if (sec_w > 100U) sec_w = 100U;
  if (sec_w > 0U) {
    batch_add(&b, 14U, 59U, sec_w, 4U, th->accent);
  }
  uint8_t tip_x = (uint8_t)(14U + ((sec_w > 2U) ? (sec_w - 2U) : 0U));
  batch_add(&b, tip_x, 58U, 2U, 6U, th->text);
  batch_submit(hud, &b, false);

  bool time_chg = force_all || (disp_h != hud->prev_saver_hour) ||
                  (disp_m != hud->prev_saver_min) ||
                  (l->rtc_valid != hud->prev_saver_rtc_valid);
  bool info_chg = force_all || (compute_saver_hash(hud) != hud->prev_saver_hash);
  if (time_chg) {
    char hh[3], mm[3];
    fmt_u8_zero2(disp_h, hh);
    fmt_u8_zero2(disp_m, mm);
    q_text(hud, 34U, 30U, hh, th->text, th->card, 2U, false, false);
    q_text(hud, 70U, 30U, mm, th->text, th->card, 2U, false, false);
    hud->prev_saver_hour      = disp_h;
    hud->prev_saver_min       = disp_m;
    hud->prev_saver_rtc_valid = l->rtc_valid;
  }
  q_text(hud, 58U, 30U, ":", ((phase & 1U) == 0U) ? th->accent : th->dim,
         th->card, 2U, false, !info_chg);
  hud->prev_saver_sec = disp_s;

  if (info_chg) {
    draw_screensaver_sysinfo(hud, true);
  }
}

/* ---- Built-in Clean Moving / Side-Eye / Blinking Eyes Screensaver (Pitch Black Background) ---- */
#define SAVER_COL_BLACK   0x0000U
#define SAVER_COL_WHITE   0xFFFFU
#define SAVER_COL_HEART   0xF8B2U /* Neon pink accent (Cat/DVD) */
#define SAVER_COL_BLUSH   0xF995U /* Cat inner ears & rosy cheeks */
#define SAVER_COL_GOLD    0xFFE0U /* DVD corner flash */
#define SAVER_COL_CYAN    0x3F5FU /* Neon cyan */
#define SAVER_COL_MINT    0x37D3U /* Neon mint */
#define SAVER_COL_AMBER   0xFDC6U /* Warm amber */
#define SAVER_COL_PURPLE  0xB19FU /* Neon violet */
#define SAVER_COL_CORAL   0xF9C9U /* Neon coral */

static void batch_add_eye_sclera(rect_batch_t *b, uint8_t cx, uint8_t cy, uint16_t col)
{
  /* 4-band smooth rounded oval sclera (fits 2 eyes + clear = 9 rects <= MAX_BATCH_RECTS) */
  batch_add(b, (uint8_t)(cx - 10U), (uint8_t)(cy - 28U), 20U, 56U, col);
  batch_add(b, (uint8_t)(cx - 16U), (uint8_t)(cy - 24U), 32U, 48U, col);
  batch_add(b, (uint8_t)(cx - 20U), (uint8_t)(cy - 18U), 40U, 36U, col);
  batch_add(b, (uint8_t)(cx - 22U), (uint8_t)(cy - 11U), 44U, 22U, col);
}

static void batch_add_eye_pupil(rect_batch_t *b, uint8_t px, uint8_t py)
{
  batch_add(b, (uint8_t)(px - 7U), (uint8_t)(py - 8U), 14U, 16U, SAVER_COL_BLACK);
  batch_add(b, (uint8_t)(px - 4U), (uint8_t)(py - 5U), 4U, 4U, SAVER_COL_WHITE);
  batch_add(b, (uint8_t)(px + 2U), (uint8_t)(py + 2U), 2U, 2U, SAVER_COL_WHITE);
}

typedef struct {
  uint8_t dur;      /* Duration in 60 ms ticks */
  int8_t  dx;       /* Target pupil X offset */
  int8_t  dy;       /* Target pupil Y offset */
  uint8_t top_lid;  /* Target upper lid drop (0..26, >=26 = closed blink) */
  uint8_t bot_lid;  /* Target lower lid rise (0..26) */
} eye_kf_t;

static int32_t smooth_lerp(int32_t a, int32_t b, uint32_t t, uint32_t dur)
{
  if (dur == 0U || t >= dur) {
    return b;
  }
  /* Integer smoothstep: u = t/dur, s = u*u*(3 - 2*u) */
  uint32_t u = (t * 256U) / dur;
  uint32_t s = (u * u * (768U - (2U * u))) >> 16U;
  return a + (((b - a) * (int32_t)s) / 256);
}

static void draw_saver_eyes_frame(pixel_hud_t *hud, bool force_draw)
{
  /* Calm ~14s loop at 60 ms/tick (~17 FPS): looks straight at you & stays, then glides smoothly */
  static eye_kf_t const k_eye_seq[] = {
    { 35U,   0,  0,  0U,  0U }, /* 2.1s calmly looking right at you */
    {  3U,   0,  0, 26U, 26U }, /* soft natural blink close */
    {  4U,   0,  0,  0U,  0U }, /* soft blink open */
    { 15U,   0,  0,  0U,  0U }, /* 0.9s pause looking at you */
    { 10U,  11, -2,  0U,  0U }, /* 0.6s smooth glide right */
    { 22U,  11, -2,  0U,  0U }, /* 1.3s hold right */
    { 16U, -11, -2,  0U,  0U }, /* 0.96s smooth glide left */
    { 22U, -11, -2,  0U,  0U }, /* 1.3s hold left */
    { 10U,   0,  0,  0U,  0U }, /* 0.6s smooth return to you */
    { 30U,   0,  0,  0U,  0U }, /* 1.8s calmly looking right at you */
    {  8U, -11,  2, 18U, 13U }, /* 0.5s narrow eyelids into side-eye */
    { 16U, -11,  2, 18U, 13U }, /* 1.0s hold side-eye left */
    { 18U,  11,  2, 18U, 13U }, /* 1.1s slow sly side-eye slide left -> right */
    { 16U,  11,  2, 18U, 13U }, /* 1.0s hold side-eye right */
    {  8U,   0,  0,  0U,  0U }, /* 0.5s open lids & look back at you */
  };
  enum { EYE_KF_COUNT = (uint32_t)(sizeof(k_eye_seq) / sizeof(k_eye_seq[0])) };

  uint32_t total_ticks = 0U;
  for (uint32_t i = 0U; i < EYE_KF_COUNT; i++) {
    total_ticks += k_eye_seq[i].dur;
  }
  uint32_t pos = hud->saver_step % total_ticks;
  uint32_t seg = 0U;
  while (seg + 1U < EYE_KF_COUNT && pos >= k_eye_seq[seg].dur) {
    pos -= k_eye_seq[seg].dur;
    seg++;
  }
  uint32_t prev_seg = (seg == 0U) ? (EYE_KF_COUNT - 1U) : (seg - 1U);
  eye_kf_t const *a = &k_eye_seq[prev_seg];
  eye_kf_t const *z = &k_eye_seq[seg];

  int8_t  dx      = (int8_t)smooth_lerp(a->dx, z->dx, pos, z->dur);
  int8_t  dy      = (int8_t)smooth_lerp(a->dy, z->dy, pos, z->dur);
  uint8_t top_lid = (uint8_t)smooth_lerp(a->top_lid, z->top_lid, pos, z->dur);
  uint8_t bot_lid = (uint8_t)smooth_lerp(a->bot_lid, z->bot_lid, pos, z->dur);
  bool    blink   = (top_lid >= 25U && bot_lid >= 25U);

  /* Zero UART traffic during calm holds when pixels are identical to last drawn frame */
  uint32_t pose_hash = ((uint32_t)(uint8_t)(dx + 64) << 24U) |
                       ((uint32_t)(uint8_t)(dy + 64) << 16U) |
                       ((uint32_t)top_lid << 8U) |
                       (uint32_t)bot_lid |
                       (blink ? 0x80000000UL : 0UL);
  if (!force_draw && pose_hash == hud->prev_saver_hash) {
    return;
  }
  hud->prev_saver_hash = pose_hash;

  uint8_t const lcx = 36U;
  uint8_t const rcx = 92U;
  uint8_t const cy  = 64U;

  /* Batch 1: Clear eye region & draw smooth rounded white scleras */
  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 10U, 34U, 108U, 60U, SAVER_COL_BLACK);
  if (blink) {
    batch_add(&b, 16U, 62U, 40U, 4U, SAVER_COL_WHITE);
    batch_add(&b, 72U, 62U, 40U, 4U, SAVER_COL_WHITE);
    batch_submit(hud, &b, true);
    return;
  }
  batch_add_eye_sclera(&b, lcx, cy, SAVER_COL_WHITE);
  batch_add_eye_sclera(&b, rcx, cy, SAVER_COL_WHITE);
  batch_submit(hud, &b, false);

  /* Batch 2: Glossy jet-black pupils + smoothly interpolated eyelids */
  batch_init(&b);
  batch_add_eye_pupil(&b, (uint8_t)(lcx + dx), (uint8_t)(cy + dy));
  batch_add_eye_pupil(&b, (uint8_t)(rcx + dx), (uint8_t)(cy + dy));
  if (top_lid > 0U) {
    batch_add(&b, 13U, 36U, 46U, top_lid, SAVER_COL_BLACK);
    batch_add(&b, 69U, 36U, 46U, top_lid, SAVER_COL_BLACK);
  }
  if (bot_lid > 0U) {
    batch_add(&b, 13U, (uint8_t)(92U - bot_lid), 46U, bot_lid, SAVER_COL_BLACK);
    batch_add(&b, 69U, (uint8_t)(92U - bot_lid), 46U, bot_lid, SAVER_COL_BLACK);
  }
  batch_submit(hud, &b, true);
}

/* ---- Built-in Bongo Cat Screensaver (Pitch Black Background) ---- */
static void draw_saver_cat_static(pixel_hud_t *hud)
{
  q_fill(hud, 0U, 0U, 128U, 128U, SAVER_COL_BLACK, false);

  rect_batch_t b;
  batch_init(&b);
  /* Cozy desk surface */
  batch_add(&b, 0U, 96U, 128U, 2U, SAVER_COL_WHITE);
  batch_add(&b, 0U, 98U, 128U, 30U, 0x10A3U);
  /* Cat ears (outer white + inner pink) */
  batch_add(&b, 30U, 22U, 14U, 16U, SAVER_COL_WHITE);
  batch_add(&b, 33U, 26U, 8U, 10U, SAVER_COL_BLUSH);
  batch_add(&b, 84U, 22U, 14U, 16U, SAVER_COL_WHITE);
  batch_add(&b, 87U, 26U, 8U, 10U, SAVER_COL_BLUSH);
  /* Chunky white cat head & body */
  batch_add(&b, 34U, 32U, 60U, 10U, SAVER_COL_WHITE);
  batch_add(&b, 26U, 38U, 76U, 58U, SAVER_COL_WHITE);
  /* Rosy cheeks & cute 'w' mouth */
  batch_add(&b, 34U, 62U, 8U, 4U, SAVER_COL_BLUSH);
  batch_add(&b, 86U, 62U, 8U, 4U, SAVER_COL_BLUSH);
  batch_add(&b, 57U, 62U, 3U, 4U, SAVER_COL_BLACK);
  batch_add(&b, 60U, 65U, 3U, 2U, SAVER_COL_BLACK);
  batch_add(&b, 63U, 62U, 2U, 4U, SAVER_COL_BLACK);
  batch_add(&b, 65U, 65U, 3U, 2U, SAVER_COL_BLACK);
  batch_add(&b, 68U, 62U, 3U, 4U, SAVER_COL_BLACK);
  batch_submit(hud, &b, false);
}

static void draw_saver_cat_frame(pixel_hud_t *hud)
{
  uint8_t phase     = (uint8_t)(hud->saver_step & 15U);
  uint8_t beat      = phase & 3U;
  bool    left_dn   = (beat == 0U || beat == 3U);
  bool    right_dn  = (beat == 2U || beat == 3U);
  bool    blink     = (phase == 14U);
  bool    tail_flip = ((phase & 2U) != 0U);

  /* Batch 1: Eyes + raised/slapped paw regions + wagging tail */
  rect_batch_t b;
  batch_init(&b);
  /* Eyes */
  batch_add(&b, 42U, 48U, 8U, 8U, SAVER_COL_WHITE);
  batch_add(&b, 78U, 48U, 8U, 8U, SAVER_COL_WHITE);
  if (blink) {
    batch_add(&b, 42U, 52U, 8U, 2U, SAVER_COL_BLACK);
    batch_add(&b, 78U, 52U, 8U, 2U, SAVER_COL_BLACK);
  } else {
    batch_add(&b, 43U, 49U, 6U, 7U, SAVER_COL_BLACK);
    batch_add(&b, 44U, 50U, 2U, 2U, SAVER_COL_WHITE);
    batch_add(&b, 79U, 49U, 6U, 7U, SAVER_COL_BLACK);
    batch_add(&b, 80U, 50U, 2U, 2U, SAVER_COL_WHITE);
  }
  /* Clear raised paw & tail boxes */
  batch_add(&b, 10U, 64U, 16U, 28U, SAVER_COL_BLACK);
  batch_add(&b, 102U, 64U, 22U, 32U, SAVER_COL_BLACK);
  if (!left_dn) {
    batch_add(&b, 12U, 68U, 14U, 12U, SAVER_COL_WHITE);
    batch_add(&b, 15U, 72U, 6U, 4U, SAVER_COL_BLUSH);
  }
  if (!right_dn) {
    batch_add(&b, 102U, 68U, 14U, 12U, SAVER_COL_WHITE);
    batch_add(&b, 107U, 72U, 6U, 4U, SAVER_COL_BLUSH);
  }
  /* Wagging tail */
  batch_add(&b, tail_flip ? 112U : 106U, 84U, 10U, 10U, SAVER_COL_WHITE);
  batch_submit(hud, &b, false);

  /* Batch 2: Mini RGB mechanical keyboard on desk + slapping paws + music notes */
  batch_init(&b);
  batch_add(&b, 26U, 78U, 76U, 18U, SAVER_COL_WHITE); /* restore lower cat belly */
  batch_add(&b, 32U, 85U, 64U, 11U, 0x2127U);         /* keyboard chassis */
  static uint16_t const k_rgb_keys[6] = {
    SAVER_COL_CYAN, SAVER_COL_MINT, SAVER_COL_AMBER,
    SAVER_COL_HEART, SAVER_COL_PURPLE, SAVER_COL_CORAL,
  };
  for (uint8_t k = 0U; k < 6U; k++) {
    bool lit = (left_dn && k < 3U) || (right_dn && k >= 3U);
    uint16_t kc = lit ? k_rgb_keys[(k + phase) % 6U] : 0x4A8EU;
    batch_add(&b, (uint8_t)(35U + (k * 10U)), 88U, 8U, 5U, kc);
  }
  if (left_dn) {
    batch_add(&b, 32U, 80U, 18U, 10U, SAVER_COL_WHITE);
    batch_add(&b, 34U, 89U, 14U, 2U, SAVER_COL_CYAN);
  }
  if (right_dn) {
    batch_add(&b, 78U, 80U, 18U, 10U, SAVER_COL_WHITE);
    batch_add(&b, 80U, 89U, 14U, 2U, SAVER_COL_HEART);
  }
  /* Clear top note area */
  batch_add(&b, 4U, 6U, 22U, 24U, SAVER_COL_BLACK);
  batch_add(&b, 102U, 6U, 22U, 24U, SAVER_COL_BLACK);
  bool has_note = left_dn || right_dn;
  batch_submit(hud, &b, !has_note);

  if (has_note) {
    uint8_t ny = (uint8_t)(18U - ((phase & 3U) * 3U));
    if (left_dn) {
      q_text(hud, 8U, ny, "*", SAVER_COL_CYAN, SAVER_COL_BLACK, 1U, false, !right_dn);
    }
    if (right_dn) {
      q_text(hud, 108U, ny, "+", SAVER_COL_HEART, SAVER_COL_BLACK, 1U, false, true);
    }
  }
}

/* ---- Built-in Matrix Digital Rain Screensaver (Pitch Black Background) ---- */
static void draw_saver_matrix_static(pixel_hud_t *hud)
{
  static uint8_t const k_seed[10] = { 0U, 5U, 10U, 2U, 7U, 12U, 4U, 9U, 1U, 6U };
  q_fill(hud, 0U, 0U, 128U, 128U, SAVER_COL_BLACK, true);
  memcpy(hud->matrix_y, k_seed, sizeof(hud->matrix_y));
}

static void draw_saver_matrix_step(pixel_hud_t *hud)
{
  static char const k_glyphs[24] = "0123456789ABCDEF$#@*+<>/";
  uint16_t step = hud->saver_step;

  for (uint8_t pass = 0U; pass < 2U; pass++) {
    uint8_t col = (uint8_t)((step + (pass * 5U)) % 10U);
    uint8_t x   = (uint8_t)(4U + (col * 12U));
    uint8_t hy  = hud->matrix_y[col];
    bool    is_last_pass = (pass == 1U);

    /* Erase tail 5 rows behind head */
    if (hy >= 5U && (hy - 5U) < 10U) {
      q_fill(hud, x, (uint8_t)(4U + ((hy - 5U) * 12U)), 6U, 12U, SAVER_COL_BLACK, false);
    }
    /* Dim trail 3 rows behind head */
    if (hy >= 3U && (hy - 3U) < 10U) {
      char g[2] = { k_glyphs[(step + col * 7U + hy) % 24U], '\0' };
      q_text(hud, x, (uint8_t)(4U + ((hy - 3U) * 12U)), g, 0x0320U, SAVER_COL_BLACK, 1U, false, false);
    }
    /* Bright phosphor green 1 row behind head */
    if (hy >= 1U && (hy - 1U) < 10U) {
      char g[2] = { k_glyphs[(step + col * 3U + hy * 5U) % 24U], '\0' };
      q_text(hud, x, (uint8_t)(4U + ((hy - 1U) * 12U)), g, 0x07E0U, SAVER_COL_BLACK, 1U, false,
             is_last_pass && (hy >= 10U));
    }
    /* Glowing white-mint head glyph */
    if (hy < 10U) {
      char g[2] = { k_glyphs[(step * 3U + col * 11U + hy) % 24U], '\0' };
      q_text(hud, x, (uint8_t)(4U + (hy * 12U)), g, 0xDFBFU, SAVER_COL_BLACK, 1U, false, is_last_pass);
    } else if (is_last_pass && (hy < 1U || (hy - 1U) >= 10U)) {
      uint8_t f = 0xFFU;
      (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, &f, 1U);
    }
    hud->matrix_y[col] = (uint8_t)((hy + 1U) % 15U);
  }
}

/* ---- Built-in Bouncing Retro TK75 DVD Logo Screensaver (Pitch Black Background) ---- */
#define DVD_BADGE_W 46U
#define DVD_BADGE_H 24U

static void draw_saver_dvd_static(pixel_hud_t *hud)
{
  q_fill(hud, 0U, 0U, 128U, 128U, SAVER_COL_BLACK, true);
  hud->dvd_x       = 24;
  hud->dvd_y       = 32;
  hud->dvd_dx      = 3;
  hud->dvd_dy      = 2;
  hud->dvd_col_idx = 0U;
  hud->dvd_corners = 0U;
}

static void draw_saver_dvd_step(pixel_hud_t *hud)
{
  static uint16_t const k_dvd_cols[6] = {
    SAVER_COL_CYAN, SAVER_COL_HEART, SAVER_COL_MINT,
    SAVER_COL_AMBER, SAVER_COL_PURPLE, SAVER_COL_CORAL,
  };

  int16_t old_x = hud->dvd_x;
  int16_t old_y = hud->dvd_y;
  int16_t nx    = (int16_t)(old_x + hud->dvd_dx);
  int16_t ny    = (int16_t)(old_y + hud->dvd_dy);
  bool hit_x    = false;
  bool hit_y    = false;

  if (nx <= 2) {
    nx = 2; hud->dvd_dx = 3; hit_x = true;
  } else if (nx >= (int16_t)(126 - DVD_BADGE_W)) {
    nx = (int16_t)(126 - DVD_BADGE_W); hud->dvd_dx = -3; hit_x = true;
  }
  if (ny <= 2) {
    ny = 2; hud->dvd_dy = 2; hit_y = true;
  } else if (ny >= (int16_t)(126 - DVD_BADGE_H)) {
    ny = (int16_t)(126 - DVD_BADGE_H); hud->dvd_dy = -2; hit_y = true;
  }

  if (hit_x || hit_y) {
    hud->dvd_col_idx = (uint8_t)((hud->dvd_col_idx + 1U) % 6U);
  }
  bool corner = hit_x && hit_y;
  if (corner) {
    hud->dvd_corners++;
  }
  hud->dvd_x = nx;
  hud->dvd_y = ny;

  uint16_t col = corner ? SAVER_COL_GOLD : k_dvd_cols[hud->dvd_col_idx];
  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, (uint8_t)old_x, (uint8_t)old_y, DVD_BADGE_W, DVD_BADGE_H, SAVER_COL_BLACK);
  /* Rounded retro neon DVD pill */
  batch_add(&b, (uint8_t)(nx + 2), (uint8_t)ny, (uint8_t)(DVD_BADGE_W - 4U), DVD_BADGE_H, col);
  batch_add(&b, (uint8_t)nx, (uint8_t)(ny + 2), DVD_BADGE_W, (uint8_t)(DVD_BADGE_H - 4U), col);
  batch_add(&b, (uint8_t)(nx + 3), (uint8_t)(ny + 2), (uint8_t)(DVD_BADGE_W - 6U), 14U, SAVER_COL_BLACK);
  batch_add(&b, (uint8_t)(nx + 8), (uint8_t)(ny + 19), (uint8_t)(DVD_BADGE_W - 16U), 2U, SAVER_COL_BLACK);
  batch_submit(hud, &b, false);

  q_text(hud, (uint8_t)(nx + 11), (uint8_t)(ny + 3), "TK75", col, SAVER_COL_BLACK, 1U, false, true);
}

/* ---- Custom Animated GIF Player Fallback Card (when no GIF in U9 or U24 < v1.3) ---- */
static void draw_saver_gif_fallback(pixel_hud_t *hud, bool needs_u24_update)
{
  hud_theme_t const *th = active_theme(hud);
  q_fill(hud, 0U, 0U, 128U, 128U, SAVER_COL_BLACK, false);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 6U, 16U, 116U, 96U, th->border);
  batch_add(&b, 8U, 18U, 112U, 92U, th->card);
  batch_add(&b, 8U, 18U, 112U, 3U, th->accent);
  batch_submit(hud, &b, false);

  q_text(hud, 28U, 28U, "CUSTOM GIF", th->accent, th->card, 1U, false, false);
  q_text(hud, 16U, 48U, needs_u24_update ? "FLASH U24 v1.3" : "NO GIF IN U9  ",
         th->warn, th->card, 1U, false, false);
  q_text(hud, 16U, 66U, "USE CLI TOOL: ", th->muted, th->card, 1U, false, false);
  q_text(hud, 14U, 80U, "tk75_tft_gif.py", th->text, th->card, 1U, false, false);
  q_text(hud, 22U, 96U, "TYPE TO WAKE", th->good, th->card, 1U, false, true);
}

static bool u24_supports_u9_gif(pixel_hud_t const *hud)
{
  return (hud->patch_major > 1U) || (hud->patch_major == 1U && hud->patch_minor >= 3U);
}

/* Fullscreen interactive 82-key Hall calibration screen (128x128, themed) */
static void draw_cal_static(pixel_hud_t *hud)
{
  hud_theme_t const *th = active_theme(hud);
  q_fill(hud, 0U, 0U, 128U, 128U, th->bg, false);

  rect_batch_t b;
  batch_init(&b);
  batch_add(&b, 0U, 0U, 128U, 18U, th->header_bg);
  batch_add(&b, 0U, 18U, 128U, 1U, th->border);
  batch_add(&b, 0U, 111U, 128U, 1U, th->border);
  batch_add(&b, 0U, 112U, 128U, 16U, th->header_bg);
  batch_add(&b, 9U, 89U, 110U, 12U, th->border);
  batch_add(&b, 10U, 90U, 108U, 10U, th->dim);
  batch_add(&b, 6U, 117U, 60U, 6U, th->dim);
  batch_submit(hud, &b, false);

  q_text(hud, 4U, 3U, "HALL CAL", th->accent, th->header_bg, 1U, false, false);
  q_text(hud, 10U, 75U, "FORCE", th->muted, th->bg, 1U, false, false);
  q_text(hud, 74U, 114U, "KNOB:ESC", th->muted, th->header_bg, 1U, false, false);
}

static void draw_cal_dynamic(pixel_hud_t *hud, bool force_all)
{
  hud_theme_t const      *th = active_theme(hud);
  pixel_hud_live_t const *l  = &hud->live;
  bool cnt_chg = force_all || (l->cal_done_count != hud->prev_cal_done);
  bool key_chg = force_all || (l->cal_key_done != hud->prev_cal_key_done) ||
                 (strncmp(l->cal_label, hud->prev_cal_label, 4U) != 0);
  bool pct_chg = force_all || (l->cal_strength_pct != hud->prev_cal_pct) || key_chg;

  if (!cnt_chg && !key_chg && !pct_chg) {
    return;
  }

  rect_batch_t b;
  batch_init(&b);

  if (key_chg) {
    uint16_t kborder = l->cal_key_done ? th->good
                     : ((l->cal_strength_pct > 0U) ? th->accent : th->border);
    batch_add(&b, 14U, 23U, 100U, 48U, kborder);
    batch_add(&b, 16U, 25U, 96U, 44U, th->card);
    batch_add(&b, 18U, 26U, 92U, 2U, l->cal_key_done ? th->good : th->border);
  }

  if (pct_chg) {
    batch_add(&b, 10U, 90U, 108U, 10U, th->dim);
    uint8_t fw = (uint8_t)(((uint32_t)l->cal_strength_pct * 108U) / 100U);
    if (fw > 108U) fw = 108U;
    if (fw > 0U) {
      uint16_t scol = l->cal_key_done ? th->good
                    : ((l->cal_strength_pct >= 65U) ? th->warn : th->accent);
      batch_add(&b, 10U, 90U, fw, 10U, scol);
    }
    batch_add(&b, 48U, 89U, 2U, 12U, th->text);
  }

  if (cnt_chg) {
    uint8_t tot = (l->cal_total_keys > 0U) ? l->cal_total_keys : 82U;
    uint8_t pw  = (uint8_t)(((uint32_t)l->cal_done_count * 60U) / tot);
    if (pw > 60U) pw = 60U;
    batch_add(&b, 6U, 117U, 60U, 6U, th->dim);
    if (pw > 0U) {
      batch_add(&b, 6U, 117U, pw, 6U, th->good);
    }
  }
  bool any_text = cnt_chg || key_chg || pct_chg;
  batch_submit(hud, &b, !any_text);

  if (cnt_chg) {
    uint8_t tot = (l->cal_total_keys > 0U) ? l->cal_total_keys : 82U;
    char cstr[6], d2[3], t2[3];
    fmt_u8_zero2(l->cal_done_count, d2);
    fmt_u8_zero2(tot, t2);
    cstr[0] = d2[0]; cstr[1] = d2[1]; cstr[2] = '/'; cstr[3] = t2[0]; cstr[4] = t2[1]; cstr[5] = '\0';
    q_text(hud, 92U, 3U, cstr, th->good, th->header_bg, 1U, false, !(key_chg || pct_chg));
    hud->prev_cal_done = l->cal_done_count;
  }

  if (key_chg) {
    char lbl[5];
    memcpy(lbl, l->cal_label, 5U);
    lbl[4] = '\0';
    size_t len   = strlen(lbl);
    uint8_t sc   = (len <= 1U) ? 3U : 2U;
    uint8_t tw   = (uint8_t)(len * 6U * sc);
    uint8_t th_h = (uint8_t)(12U * sc);
    uint8_t tx   = (tw < 96U) ? (uint8_t)(16U + ((96U - tw) / 2U)) : 18U;
    uint8_t ty   = (th_h < 44U) ? (uint8_t)(25U + ((44U - th_h) / 2U)) : 28U;
    uint16_t kcol = l->cal_key_done ? th->good : th->text;
    q_text(hud, tx, ty, lbl, kcol, th->card, sc, false, !pct_chg);
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
    q_text(hud, 94U, 75U, pstr, l->cal_key_done ? th->good : th->accent, th->bg, 1U, false, true);
    hud->prev_cal_pct = l->cal_strength_pct;
  }
}

static void invalidate_render_cache(pixel_hud_t *hud)
{
  hud->prev_header_hash     = 0U;
  hud->prev_badges_hash     = 0xFFU;
  hud->prev_wpm             = 0xFFFFU;
  hud->prev_travel_disp     = 0xFFFFU;
  hud->prev_home_hour       = 0xFFU;
  hud->prev_home_min        = 0xFFU;
  hud->prev_home_sec_odd    = 0xFFU;
  hud->prev_saver_hash      = 0U;
  hud->prev_saver_hour      = 0xFFU;
  hud->prev_saver_min       = 0xFFU;
  hud->prev_saver_sec       = 0xFFU;
  hud->full_redraw_needed   = true;
}

/* Instant, zero-animation page switch */
static void switch_page_instant(pixel_hud_t *hud, uint8_t new_page)
{
  hud->page               = (uint8_t)(new_page % PIXEL_PAGE_COUNT);
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
  hud->applied_theme      = (cfg != NULL) ? normalize_theme_id(cfg->tft_theme) : 0U;
  hud->applied_tft_brt    = 0xFFU;
  hud->macro_play_req     = 0xFFU;
  invalidate_render_cache(hud);
}

bool pixel_hud_is_active(pixel_hud_t const *hud)
{
  return (hud != NULL) && hud->detected;
}

bool pixel_hud_is_menu_open(pixel_hud_t const *hud)
{
  return (hud != NULL) && hud->menu_open;
}

uint8_t pixel_hud_get_theme(pixel_hud_t const *hud)
{
  return (hud != NULL && hud->cfg != NULL) ? normalize_theme_id(hud->cfg->tft_theme) : 0U;
}

void pixel_hud_on_rx(pixel_hud_t *hud, uart7_rx_frame_t const *frame)
{
  if (hud == NULL || frame == NULL) {
    return;
  }
  if (frame->cmd >= 0xA0U && frame->cmd <= 0xA9U) {
    hud->awaiting_ack = false;
  }
  if (frame->cmd == 0xA9U && frame->payload_len >= 5U && frame->payload[0] == 0x00U) {
    hud->u9_gif_probed = true;
    hud->u9_gif_valid  = (frame->payload[1] != 0U) && (frame->payload[2] > 0U);
    hud->u9_gif_frames = frame->payload[2];
    uint16_t dms = (uint16_t)frame->payload[3] | ((uint16_t)frame->payload[4] << 8U);
    if (dms < 33U) dms = 33U;
    if (dms > 10000U) dms = 10000U;
    hud->u9_gif_delay_ms = dms;
  } else if (frame->cmd == 0xA9U && frame->payload_len == 4U && frame->payload[0] == 0x00U) {
    /* U24 v1.4+ per-frame hold duration returned with each frame draw ACK */
    uint16_t dms = (uint16_t)frame->payload[2] | ((uint16_t)frame->payload[3] << 8U);
    if (dms < 33U) dms = 33U;
    if (dms > 10000U) dms = 10000U;
    hud->u9_gif_delay_ms = dms;
  }
  if (frame->cmd == 0xA5U && frame->payload_len >= 3U && frame->payload[0] == 0x00U) {
    hud->patch_major = frame->payload[1];
    hud->patch_minor = frame->payload[2];
    if (!hud->detected) {
      hud->detected           = true;
      hud->applied_theme      = (hud->cfg != NULL) ? normalize_theme_id(hud->cfg->tft_theme) : 0U;
      hud->tft_brt_dirty      = true;
      hud->applied_tft_brt    = 0xFFU;
      hud->brt_reapply_left   = 3U;
      hud->brt_reapply_armed  = false;
      invalidate_render_cache(hud);
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
  hud->screensaver_active = false;
  hud->menu_open          = true;
  hud->editing            = false;
  hud->focus_row          = 1U; /* Jump straight to TFT brightness slider for 1-click access */
  hud->last_input_ms      = now_ms;
  switch_page_instant(hud, PIXEL_PAGE_BRIGHTNESS);
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
  switch_page_instant(hud, PIXEL_PAGE_HOME);
}

static uint8_t find_nearest_eco_idx(uint8_t mins)
{
  uint8_t best = 0U;
  for (uint8_t i = 0U; i < ECO_PRESET_COUNT; i++) {
    if (mins >= k_eco_presets[i]) {
      best = i;
    }
  }
  return best;
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
      c->tft_brightness  = (uint8_t)v;
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
      /* Support all 4 SOCD modes consistently with VIA & KC_SOCD_CYCLE */
      int32_t m = ((int32_t)c->hall_socd_mode + dir + 4) % 4;
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
  } else if (hud->page == PIXEL_PAGE_POWER) {
    if (hud->focus_row == 1U) {
      int32_t idx = ((int32_t)find_nearest_eco_idx(c->sleep_timeout_min) + dir +
                     (int32_t)ECO_PRESET_COUNT) % (int32_t)ECO_PRESET_COUNT;
      c->sleep_timeout_min = k_eco_presets[idx];
      return true;
    }
    if (hud->focus_row == 2U) {
      int32_t cur = (int32_t)normalize_saver_mode(c->tft_screensaver);
      int32_t nxt = (cur + dir + (int32_t)TFT_SAVER_COUNT) % (int32_t)TFT_SAVER_COUNT;
      c->tft_screensaver = (uint8_t)nxt;
      return true;
    }
    if (hud->focus_row == 3U) {
      int32_t r = ((int32_t)(c->rf_poll_rate_idx & 3U) + dir + 4) % 4;
      c->rf_poll_rate_idx = (uint8_t)r;
      return true;
    }
    if (hud->focus_row == 4U) {
      c->tft_clock_24h ^= 1U;
      return true;
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
      c->win_lock ^= 1U;
      return true;
    }
    if (hud->focus_row == 4U) {
      /* Cycle persistent TFT skin cleanly: PIXEL <-> STOCK+ <-> TERM */
      int32_t cur = (int32_t)normalize_theme_id(c->tft_theme);
      int32_t nxt = (cur + dir + (int32_t)TFT_THEME_COUNT) % (int32_t)TFT_THEME_COUNT;
      c->tft_theme = (uint8_t)nxt;
      invalidate_render_cache(hud);
      return true;
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
    hud->body_redraw_needed = true;
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

  /* Not editing: PREV / NEXT navigates tab bar or rows (zero animation!) */
  if (hud->focus_row == 0U || hud->page == PIXEL_PAGE_HOME) {
    int32_t next_p = ((int32_t)hud->page + (int32_t)PIXEL_PAGE_COUNT + (int32_t)dir) % (int32_t)PIXEL_PAGE_COUNT;
    hud->focus_row = 0U;
    switch_page_instant(hud, (uint8_t)next_p);
  } else {
    int32_t max_r  = (hud->page == PIXEL_PAGE_MACRO) ? 3 : 4;
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

  /* Detect external theme switches (e.g. VIA ch=7 val=10) & force spotless full repaint */
  uint8_t desired_theme = normalize_theme_id(hud->cfg->tft_theme);
  if (desired_theme != hud->applied_theme) {
    hud->applied_theme = desired_theme;
    invalidate_render_cache(hud);
    if (hud->screensaver_active) {
      hud->screensaver_active = false;
    }
    if (hud->prev_cal_active) {
      hud->prev_cal_active = false;
    }
    if (hud->prev_macro_rec) {
      hud->prev_macro_rec = false;
    }
    if (hud->prev_macro_rev) {
      hud->prev_macro_rev = false;
    }
  }

  /* Drain queued draw ops first */
  pump_tx_queue(hud, now_ms);

  /* Menu auto-timeout */
  if (hud->menu_open && (uint32_t)(now_ms - hud->last_input_ms) >= MENU_TIMEOUT_MS) {
    pixel_hud_close_menu(hud);
  }

  /* Post-boot brightness + display flush re-assert (+300ms, +900ms, +1800ms) */
  if (hud->brt_reapply_left > 0U) {
    if (!hud->brt_reapply_armed) {
      hud->brt_reapply_armed = true;
      hud->brt_reapply_ms    = now_ms + 300U;
    } else if ((int32_t)(now_ms - hud->brt_reapply_ms) >= 0) {
      hud->applied_tft_brt = 0xFFU;
      hud->tft_brt_dirty   = true;
      if (hud->brt_reapply_left == 3U && !hud->menu_open && !hud->screensaver_active &&
          !hud->live.macro_recording && !hud->live.macro_reviewing &&
          !hud->live.cal_active && !hud->live.any_actuated) {
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
        hud->brt_reapply_armed = false;
      }
    }
  }

  if (!hud->full_redraw_needed && hud->tft_brt_dirty && hud->q_count < (PIXEL_HUD_QUEUE_CAP - 2U)) {
    hud->tft_brt_dirty = false;
    if (hud->cfg->tft_brightness != hud->applied_tft_brt) {
      hud->applied_tft_brt = hud->cfg->tft_brightness;
      q_brightness_pct(hud, hud->applied_tft_brt);
      pump_tx_queue(hud, now_ms);
    }
  }

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
    hud->prev_cal_active = false;
    hud->page            = PIXEL_PAGE_HOME;
    invalidate_render_cache(hud);
  }

  /* Fullscreen macro text editor takeover */
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
    hud->prev_macro_rev  = false;
    hud->edit_dirty_mask = 0U;
    invalidate_render_cache(hud);
  }

  /* Fullscreen macro recording takeover */
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
    hud->prev_macro_rec = false;
    invalidate_render_cache(hud);
  }

  /* Selectable Idle Screensaver (Clock, Funny Eyes, Bongo Cat, Matrix, DVD, Custom U9 GIF) */
  uint8_t saver_mode = normalize_saver_mode(hud->cfg->tft_screensaver);
  bool saver_want = (saver_mode != (uint8_t)TFT_SAVER_OFF) && !hud->menu_open &&
                    (hud->page == PIXEL_PAGE_HOME) &&
                    (hud->live.idle_ms >= SCREENSAVER_IDLE_MS) &&
                    !hud->live.any_actuated;
  if (saver_want) {
    if (!hud->screensaver_active || hud->saver_mode_rendered != saver_mode) {
      hud->screensaver_active    = true;
      hud->saver_mode_rendered   = saver_mode;
      hud->saver_phase           = 0U;
      hud->saver_step            = 0U;
      hud->prev_saver_hour       = 0xFFU;
      hud->prev_saver_min        = 0xFFU;
      hud->prev_saver_sec        = 0xFFU;
      hud->prev_saver_rtc_valid  = false;
      hud->u9_gif_probed         = false;
      hud->u9_gif_valid          = false;
      hud->u9_gif_fallback_drawn = false;
      hud->u9_gif_cur_frame      = 0U;
      hud->u9_gif_probe_ms       = now_ms;
      hud->last_anim_ms          = now_ms;

      switch (saver_mode) {
        case TFT_SAVER_CLOCK:
          draw_screensaver_static(hud);
          draw_screensaver_anim(hud, true);
          break;
        case TFT_SAVER_EYES:
          q_fill(hud, 0U, 0U, 128U, 128U, SAVER_COL_BLACK, false);
          draw_saver_eyes_frame(hud, true);
          break;
        case TFT_SAVER_CAT:
          draw_saver_cat_static(hud);
          draw_saver_cat_frame(hud);
          break;
        case TFT_SAVER_MATRIX:
          draw_saver_matrix_static(hud);
          draw_saver_matrix_step(hud);
          break;
        case TFT_SAVER_DVD:
          draw_saver_dvd_static(hud);
          draw_saver_dvd_step(hud);
          break;
        case TFT_SAVER_GIF:
          if (u24_supports_u9_gif(hud)) {
            uint8_t qop = 0U;
            (void)enqueue_op(hud, PIXEL_HUD_CMD_GIF_PLAY, &qop, 1U);
          } else {
            draw_saver_gif_fallback(hud, true);
            hud->u9_gif_fallback_drawn = true;
          }
          break;
        default:
          break;
      }
      pump_tx_queue(hud, now_ms);
      return;
    }

    if (saver_mode == (uint8_t)TFT_SAVER_CLOCK) {
      if ((uint32_t)(now_ms - hud->last_anim_ms) >= 500U) {
        hud->last_anim_ms = now_ms;
        hud->saver_phase  = (uint8_t)((hud->saver_phase + 1U) & 3U);
        draw_screensaver_anim(hud, false);
        pump_tx_queue(hud, now_ms);
      }
    } else if (saver_mode == (uint8_t)TFT_SAVER_EYES) {
      if ((uint32_t)(now_ms - hud->last_anim_ms) >= 60U) {
        hud->last_anim_ms = now_ms;
        hud->saver_step++;
        draw_saver_eyes_frame(hud, false);
        pump_tx_queue(hud, now_ms);
      }
    } else if (saver_mode == (uint8_t)TFT_SAVER_CAT) {
      if ((uint32_t)(now_ms - hud->last_anim_ms) >= 140U) {
        hud->last_anim_ms = now_ms;
        hud->saver_step++;
        draw_saver_cat_frame(hud);
        pump_tx_queue(hud, now_ms);
      }
    } else if (saver_mode == (uint8_t)TFT_SAVER_MATRIX) {
      if ((uint32_t)(now_ms - hud->last_anim_ms) >= 100U) {
        hud->last_anim_ms = now_ms;
        hud->saver_step++;
        draw_saver_matrix_step(hud);
        pump_tx_queue(hud, now_ms);
      }
    } else if (saver_mode == (uint8_t)TFT_SAVER_DVD) {
      if ((uint32_t)(now_ms - hud->last_anim_ms) >= 65U) {
        hud->last_anim_ms = now_ms;
        hud->saver_step++;
        draw_saver_dvd_step(hud);
        pump_tx_queue(hud, now_ms);
      }
    } else if (saver_mode == (uint8_t)TFT_SAVER_GIF) {
      if (hud->u9_gif_probed && hud->u9_gif_valid && hud->u9_gif_frames > 0U) {
        uint16_t period = (hud->u9_gif_delay_ms >= 33U) ? hud->u9_gif_delay_ms : 100U;
        if (hud->saver_step == 0U ||
            (hud->u9_gif_frames > 1U && (uint32_t)(now_ms - hud->last_anim_ms) >= period)) {
          uint8_t cmdp[2] = { 1U, hud->u9_gif_cur_frame };
          (void)enqueue_op(hud, PIXEL_HUD_CMD_GIF_PLAY, cmdp, 2U);
          hud->u9_gif_cur_frame = (uint8_t)((hud->u9_gif_cur_frame + 1U) % hud->u9_gif_frames);
          hud->saver_step++;
          hud->last_anim_ms = now_ms;
          pump_tx_queue(hud, now_ms);
        }
      } else if (!hud->u9_gif_fallback_drawn &&
                 (hud->u9_gif_probed || (uint32_t)(now_ms - hud->u9_gif_probe_ms) >= 400U)) {
        draw_saver_gif_fallback(hud, !u24_supports_u9_gif(hud));
        hud->u9_gif_fallback_drawn = true;
        pump_tx_queue(hud, now_ms);
      }
    }
    return;
  } else if (hud->screensaver_active) {
    hud->screensaver_active = false;
    invalidate_render_cache(hud);
  }

  if (hud->full_redraw_needed || hud->body_redraw_needed) {
    hud->full_redraw_needed = false;
    hud->body_redraw_needed = false;
    render_full_page(hud);
    pump_tx_queue(hud, now_ms);
    return;
  }

  /* Live HOME page incremental updates (~35 FPS smooth switch animation, zero traffic when idle!) */
  if (hud->page == PIXEL_PAGE_HOME && (uint32_t)(now_ms - hud->last_anim_ms) >= ANIM_PERIOD_MS) {
    hud->last_anim_ms = now_ms;

    uint16_t disp_travel = update_display_travel(hud);
    uint8_t  target_px   = (uint8_t)(((uint32_t)disp_travel * PLUNGER_MAX_PX) / 4000U);
    if (target_px > PLUNGER_MAX_PX) target_px = PLUNGER_MAX_PX;

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

    bool hdr_chg = (compute_header_hash(hud) != hud->prev_header_hash);
    if (hdr_chg) {
      draw_home_header(hud);
    }

    if (active_theme(hud)->id == TFT_THEME_STOCK_PLUS) {
      uint8_t hh = 0U, mm = 0U, ss = 0U;
      get_display_hms(hud, &hh, &mm, &ss);
      uint16_t td = (disp_travel > 0U)
                      ? (uint16_t)((disp_travel + 50U) / 100U)
                      : (uint16_t)((hud->cfg->hall_actuation_um + 50U) / 100U);
      bool body_chg = (hh != hud->prev_home_hour) || (mm != hud->prev_home_min) ||
                      ((ss & 1U) != hud->prev_home_sec_odd) ||
                      (hud->live.rtc_valid != hud->prev_home_rtc_valid) ||
                      (hud->live.wpm != hud->prev_wpm) ||
                      (td != hud->prev_travel_disp) ||
                      (next_px != hud->anim_plunger_px) ||
                      (hud->live.any_actuated != hud->prev_actuated) ||
                      (compute_badges_hash(hud) != hud->prev_badges_hash);
      if (body_chg) {
        draw_stock_home_dynamic(hud, next_px, hud->live.any_actuated, false, true);
      } else if (hdr_chg) {
        uint8_t f = 0xFFU;
        (void)enqueue_op(hud, PIXEL_HUD_CMD_BRIGHTNESS, &f, 1U);
      }
      pump_tx_queue(hud, now_ms);
      return;
    }

    bool sw_chg = (next_px != hud->anim_plunger_px) || (hud->live.any_actuated != hud->prev_actuated);
    uint16_t td = (disp_travel > 0U)
                    ? (uint16_t)((disp_travel + 50U) / 100U)
                    : (uint16_t)((hud->cfg->hall_actuation_um + 50U) / 100U);
    bool stats_chg = (hud->live.wpm != hud->prev_wpm) || (td != hud->prev_travel_disp) ||
                     (compute_badges_hash(hud) != hud->prev_badges_hash);

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
