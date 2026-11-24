#include "rgb_effects.h"
#include "keymap.h"
#include "macro_recorder.h"
#include <string.h>

/* Serpentine hardware LED index per (row, col) verified from 0x08020256 in tk75hev2_iso_v506_plain */
static uint8_t const k_matrix_led_map[HALL_ROWS][HALL_COLS] = {
  {   0,   1,   2,   3,   4,   5,   6,   7,   8,   9,  10,  11,  12,  13, 255 },
  {  28,  27,  26,  25,  24,  23,  22,  21,  20,  19,  18,  17,  16,  15,  14 },
  {  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41, 255,  42 },
  {  57,  56,  55,  54,  53,  52,  51,  50,  49,  48,  47,  46,  45,  44,  43 },
  {  58,  59,  60,  61,  62,  63,  64,  65,  66,  67,  68,  69,  70,  71, 255 },
  {  81,  80,  79, 255, 255, 255,  78, 255, 255,  77,  76,  75,  74,  73,  72 },
};

static uint8_t scale8(uint8_t val, uint8_t scale)
{
  return (uint8_t)(((uint16_t)val * (uint16_t)scale) >> 8U);
}

static uint8_t triangle_wave8(uint8_t phase)
{
  return (phase < 128U) ? (uint8_t)(phase * 2U) : (uint8_t)((255U - phase) * 2U);
}

rgb_grb_t rgb_hsv_to_grb(uint8_t hue, uint8_t sat, uint8_t val)
{
  rgb_grb_t out = { val, val, val };
  if (sat == 0U || val == 0U) {
    if (val == 0U) {
      out.g = out.r = out.b = 0U;
    }
    return out;
  }

  /* Bit-identical to hue / 43U for all 0..255 with zero integer division */
  uint8_t region    = (uint8_t)(((uint32_t)hue * 6097U) >> 18U);
  uint8_t remainder = (uint8_t)((hue - (region * 43U)) * 6U);
  uint8_t p = (uint8_t)(((uint16_t)val * (255U - sat)) >> 8U);
  uint8_t q = (uint8_t)(((uint16_t)val * (255U - (((uint16_t)sat * remainder) >> 8U))) >> 8U);
  uint8_t t = (uint8_t)(((uint16_t)val * (255U - (((uint16_t)sat * (255U - remainder)) >> 8U))) >> 8U);

  switch (region) {
    case 0U: out.r = val; out.g = t;   out.b = p;   break;
    case 1U: out.r = q;   out.g = val; out.b = p;   break;
    case 2U: out.r = p;   out.g = val; out.b = t;   break;
    case 3U: out.r = p;   out.g = q;   out.b = val; break;
    case 4U: out.r = t;   out.g = p;   out.b = val; break;
    default: out.r = val; out.g = p;   out.b = q;   break;
  }
  return out;
}

uint8_t rgb_matrix_to_led_idx(uint8_t row, uint8_t col)
{
  if (row >= HALL_ROWS || col >= HALL_COLS) {
    return 0xFFU;
  }
  return k_matrix_led_map[row][col];
}

static bool key_matches_macro_slot(tk75_persist_cfg_t const *cfg, uint8_t row, uint8_t col, uint8_t slot)
{
  uint16_t target = MACRO(slot);
  return cfg->keymap[1][row][col] == target || cfg->keymap[0][row][col] == target;
}

typedef struct {
  uint8_t   phase;
  uint32_t  layer_mask;
  bool      fn_held;
  uint16_t  macro_pop_mask;
  rgb_grb_t uniform_base;
  rgb_grb_t macro_hl_col;
} rgb_frame_cache_t;

static uint16_t build_macro_populated_mask(uint8_t const *macros)
{
  uint16_t mask = 0U;
  uint16_t off  = 0U;
  for (uint8_t slot = 0U; slot < TK75_MACRO_COUNT && off < TK75_MACRO_BUF_BYTES; slot++) {
    if (macros[off] != 0U) {
      mask |= (uint16_t)(1U << slot);
    }
    while (off < TK75_MACRO_BUF_BYTES && macros[off] != 0U) {
      off++;
    }
    if (off < TK75_MACRO_BUF_BYTES) {
      off++;
    }
  }
  return mask;
}

static rgb_grb_t render_overlay_layer_color(rgb_render_ctx_t const *ctx,
                                            rgb_frame_cache_t const *fc,
                                            uint8_t layer,
                                            uint16_t layer_kc,
                                            uint8_t bright)
{
  tk75_persist_cfg_t const *cfg = ctx->cfg;
  if (layer == 1U) {
    /* Layer 1 (Fn) high-contrast palette guaranteed distinct from base backlight & each other:
     * - Rainbow wave base: Crisp White for Fn keys, Emerald Green for populated macros.
     * - White / pastel base (sat < 128): Vivid Orange for Fn keys, Emerald Green for macros.
     * - Colored base (sat >= 128): Equilateral triad (+85 = 120 deg Fn, +171 = 240 deg Macros). */
    bool is_populated_macro =
        ((layer_kc & 0xFFF0U) == QK_MACRO_BASE) &&
        ((fc->macro_pop_mask & (uint16_t)(1U << (layer_kc & 0x0FU))) != 0U);

    if (cfg->rgb_effect == RGB_FX_RAINBOW_WAVE) {
      return is_populated_macro ? rgb_hsv_to_grb(85U, 255U, bright)
                                : rgb_hsv_to_grb(0U, 0U, bright);
    }
    if (cfg->rgb_sat < 128U) {
      return is_populated_macro ? rgb_hsv_to_grb(85U, 255U, bright)
                                : rgb_hsv_to_grb(20U, 255U, bright);
    }
    uint8_t hue = is_populated_macro ? (uint8_t)(cfg->rgb_hue + 171U)
                                     : (uint8_t)(cfg->rgb_hue + 85U);
    return rgb_hsv_to_grb(hue, 255U, bright);
  }

  /* L2 (macOS): exact 180 deg complement (+128); L3 (Custom): triadic complement (+85) */
  uint8_t offset = (layer == 2U) ? 128U : 85U;
  uint8_t comp_h = (uint8_t)(cfg->rgb_hue + offset);
  uint8_t comp_s = (cfg->rgb_sat >= 64U) ? cfg->rgb_sat : 200U;
  return rgb_hsv_to_grb(comp_h, comp_s, bright);
}

static uint8_t resolve_overlay_highlight_layer(rgb_render_ctx_t const *ctx,
                                               uint32_t mask,
                                               uint8_t row,
                                               uint8_t col,
                                               uint16_t *out_kc)
{
  tk75_persist_cfg_t const *cfg = ctx->cfg;
  if ((mask & ~1UL) == 0U) {
    return 0U;
  }

  for (int8_t l = (int8_t)(TK75_KEYMAP_LAYERS - 1U); l >= 1; l--) {
    if ((mask & (1UL << (uint8_t)l)) == 0U) {
      continue;
    }
    uint16_t lkc = cfg->keymap[l][row][col];
    if (lkc > KC_TRNS) {
      *out_kc = lkc;
      return (uint8_t)l;
    }
    if (lkc == KC_NO) {
      return 0U;
    }
    /* Highlight the MO(l) key itself while its layer is held */
    if (cfg->keymap[0][row][col] == (uint16_t)(QK_MO_BASE | (uint16_t)l)) {
      *out_kc = cfg->keymap[0][row][col];
      return (uint8_t)l;
    }
  }
  return 0U;
}

static uint8_t blend_u8(uint8_t base, uint8_t target, uint8_t alpha)
{
  int32_t diff = (int32_t)target - (int32_t)base;
  if (diff >= 0) {
    uint32_t p = (uint32_t)diff * (uint32_t)alpha;
    return (uint8_t)((uint32_t)base + ((p + 1U + (p >> 8U)) >> 8U));
  }
  uint32_t p = (uint32_t)(-diff) * (uint32_t)alpha;
  return (uint8_t)((uint32_t)base - ((p + 1U + (p >> 8U)) >> 8U));
}

static rgb_grb_t blend_grb(rgb_grb_t base, rgb_grb_t target, uint8_t alpha)
{
  rgb_grb_t out = {
    .g = blend_u8(base.g, target.g, alpha),
    .r = blend_u8(base.r, target.r, alpha),
    .b = blend_u8(base.b, target.b, alpha),
  };
  return out;
}

static bool key_is_macro_typed(rgb_render_ctx_t const *ctx, uint8_t row, uint8_t col)
{
  if (ctx->macro_typed_bitmap == NULL) {
    return false;
  }
  tk75_persist_cfg_t const *cfg = ctx->cfg;
  uint16_t kc = cfg->keymap[0][row][col];
  if (cfg->mac_mode != 0U && cfg->keymap[2][row][col] != KC_TRNS) {
    kc = cfg->keymap[2][row][col];
  }
  if (kc == 0U || kc > 0xFFU) {
    return false;
  }
  uint8_t code = (uint8_t)kc;
  return (ctx->macro_typed_bitmap[code >> 3U] & (uint8_t)(1U << (code & 7U))) != 0U;
}

static rgb_grb_t render_base_effect_color(rgb_render_ctx_t const *ctx,
                                          rgb_frame_cache_t const *fc,
                                          uint8_t row,
                                          uint8_t col,
                                          uint8_t led_idx)
{
  tk75_persist_cfg_t const *cfg = ctx->cfg;
  uint8_t bright = cfg->rgb_brightness;

  switch (cfg->rgb_effect) {
    case RGB_FX_RAINBOW_WAVE: {
      uint8_t h = (uint8_t)(cfg->rgb_hue + (col * 14U) + (row * 10U) - fc->phase);
      return rgb_hsv_to_grb(h, cfg->rgb_sat, bright);
    }
    case RGB_FX_SOLID:
    case RGB_FX_BREATHING:
      return fc->uniform_base;

    case RGB_FX_HE_ANALOG_DEPTH: {
      uint16_t travel  = hall_engine_get_travel_um(ctx->hall, row, col);
      /* Idle dim base + progressive cyan->magenta->white flare proportional to Hall travel */
      uint8_t floor_v = (uint8_t)(bright >> 3U);
      uint8_t dyn_v   = (uint8_t)(((uint32_t)travel * (uint32_t)(bright - floor_v)) / HALL_MAX_TRAVEL_UM);
      uint8_t h       = (uint8_t)(cfg->rgb_hue + ((uint32_t)travel * 96U / HALL_MAX_TRAVEL_UM));
      uint8_t sat     = (travel > 3400U) ? 80U : cfg->rgb_sat;
      return rgb_hsv_to_grb(h, sat, (uint8_t)(floor_v + dyn_v));
    }
    case RGB_FX_REACTIVE_HEAT: {
      uint8_t heat = ctx->fx->heatmap[led_idx];
      uint8_t v = scale8(heat, bright);
      uint8_t h = (uint8_t)(cfg->rgb_hue + ((255U - heat) >> 2U));
      return rgb_hsv_to_grb(h, cfg->rgb_sat, v);
    }
    case RGB_FX_LAYER_INDICATOR: {
      uint16_t kc = keymap_lookup(cfg, (1UL << ctx->active_layer) | 1UL, row, col);
      uint8_t v = (ctx->active_layer > 0U && cfg->keymap[ctx->active_layer][row][col] > KC_TRNS)
                      ? bright
                      : (uint8_t)(bright >> 3U);
      if (kc == KC_NO) {
        v = 0U;
      }
      return rgb_hsv_to_grb((uint8_t)(cfg->rgb_hue + ctx->active_layer * 48U), cfg->rgb_sat, v);
    }
    default:
      return (rgb_grb_t){ 0U, 0U, 0U };
  }
}

static rgb_grb_t render_key_color(rgb_render_ctx_t const *ctx,
                                  rgb_frame_cache_t const *fc,
                                  uint8_t row,
                                  uint8_t col,
                                  uint8_t led_idx)
{
  tk75_persist_cfg_t const *cfg    = ctx->cfg;
  uint8_t                   bright = cfg->rgb_brightness;
  bool                      pressed = hall_engine_is_pressed(ctx->hall, row, col);

  if (pressed) {
    ctx->fx->heatmap[led_idx] = 255U;
  } else if (ctx->fx->heatmap[led_idx] > 8U) {
    ctx->fx->heatmap[led_idx] = (uint8_t)(ctx->fx->heatmap[led_idx] - 8U);
  } else {
    ctx->fx->heatmap[led_idx] = 0U;
  }

  /* Live macro typing glow tracker (latches fast taps so every typed letter illuminates cleanly) */
  if (key_is_macro_typed(ctx, row, col)) {
    ctx->fx->macro_glow[led_idx] = 255U;
  } else if (ctx->fx->macro_glow[led_idx] > 28U) {
    ctx->fx->macro_glow[led_idx] = (uint8_t)(ctx->fx->macro_glow[led_idx] - 28U);
  } else {
    ctx->fx->macro_glow[led_idx] = 0U;
  }

  /* Xtrfy K5-style macro recording blink (red) & save confirm flash (mint) */
  if (ctx->macro_rec_active &&
      (key_matches_macro_slot(cfg, row, col, ctx->macro_rec_slot) ||
       cfg->keymap[0][row][col] == MO(1))) {
    bool blink_on = ((ctx->now_ms >> 7U) & 1U) != 0U;
    return blink_on ? rgb_hsv_to_grb(0U, 255U, bright) : (rgb_grb_t){ 0U, 0U, 0U };
  }
  if (ctx->macro_saved_flash && key_matches_macro_slot(cfg, row, col, ctx->macro_saved_slot)) {
    return rgb_hsv_to_grb(96U, 255U, bright);
  }

  rgb_grb_t color;
  uint16_t  ov_kc    = KC_NO;
  uint8_t   ov_layer = resolve_overlay_highlight_layer(ctx, fc->layer_mask, row, col, &ov_kc);
  if (ov_layer > 0U) {
    color = render_overlay_layer_color(ctx, fc, ov_layer, ov_kc, bright);
  } else {
    color = render_base_effect_color(ctx, fc, row, col, led_idx);
    /* Dim unbound keys to 25% while Fn (Layer 1) is held so Fn & Macro keys pop unmistakably */
    if (fc->fn_held) {
      color.g >>= 2U;
      color.r >>= 2U;
      color.b >>= 2U;
    }
  }

  uint8_t mglow = ctx->fx->macro_glow[led_idx];
  if (mglow > 0U) {
    color = blend_grb(color, fc->macro_hl_col, mglow);
  }
  return color;
}

static void render_sidelight(rgb_render_ctx_t const *ctx, rgb_grb_t out_leds[RGB_LED_COUNT])
{
  tk75_persist_cfg_t const *cfg = ctx->cfg;

  /* Optional Caps Lock sidelight trigger: overrides SID even when side_enabled == 0 */
  if (cfg->caps_side_enabled != 0U && (ctx->host_leds & 0x02U) != 0U) {
    uint8_t base_v = (cfg->side_brightness >= 40U) ? cfg->side_brightness
                   : ((cfg->rgb_brightness >= 40U) ? cfg->rgb_brightness : 160U);
    uint8_t fx = (cfg->caps_side_effect < CAPS_SIDE_FX_COUNT)
                   ? cfg->caps_side_effect
                   : (uint8_t)CAPS_SIDE_FX_SOLID;
    uint32_t phase = (ctx->now_ms * ((uint32_t)cfg->side_speed + 48U)) >> 9U;
    uint8_t  scan_step = (uint8_t)((ctx->now_ms / 80U) % 14U);
    uint8_t  scan_pos  = (scan_step < 8U) ? scan_step : (uint8_t)(14U - scan_step);
    bool     blink_on  = (((ctx->now_ms / 250U) & 1U) == 0U);

    for (uint8_t i = 0U; i < RGB_SIDE_LED_COUNT; i++) {
      uint8_t h = cfg->side_hue;
      uint8_t v = base_v;
      if (fx == CAPS_SIDE_FX_BREATHING) {
        uint8_t floor_v = (uint8_t)(base_v / 6U);
        uint8_t wave_v  = scale8(triangle_wave8((uint8_t)(phase & 0xFFU)), (uint8_t)(base_v - floor_v));
        v               = (uint8_t)(floor_v + wave_v);
      } else if (fx == CAPS_SIDE_FX_RAINBOW_WAVE) {
        h = (uint8_t)(h + (i * 28U) + (phase & 0xFFU));
      } else if (fx == CAPS_SIDE_FX_BLINK) {
        v = blink_on ? base_v : 0U;
      } else if (fx == CAPS_SIDE_FX_SCANNER) {
        uint8_t dist = (i >= scan_pos) ? (uint8_t)(i - scan_pos) : (uint8_t)(scan_pos - i);
        if (dist == 0U) {
          v = base_v;
        } else if (dist == 1U) {
          v = (uint8_t)(base_v / 3U);
        } else if (dist == 2U) {
          v = (uint8_t)(base_v / 10U);
        } else {
          v = 0U;
        }
      }
      out_leds[RGB_KEY_LED_COUNT + i] = rgb_hsv_to_grb(h, cfg->side_sat, v);
    }
    return;
  }

  if (cfg->side_enabled == 0U || cfg->side_effect == RGB_FX_OFF) {
    memset(&out_leds[RGB_KEY_LED_COUNT], 0, RGB_SIDE_LED_COUNT * sizeof(rgb_grb_t));
    return;
  }

  uint32_t phase = (ctx->now_ms * ((uint32_t)cfg->side_speed + 32U)) >> 9U;
  for (uint8_t i = 0U; i < RGB_SIDE_LED_COUNT; i++) {
    uint8_t h = cfg->side_hue;
    uint8_t v = cfg->side_brightness;
    if (cfg->side_effect == RGB_FX_RAINBOW_WAVE) {
      h = (uint8_t)(h + (i * 28U) + (phase & 0xFFU));
    } else if (cfg->side_effect == RGB_FX_BREATHING) {
      v = scale8(triangle_wave8((uint8_t)(phase & 0xFFU)), v);
    }
    out_leds[RGB_KEY_LED_COUNT + i] = rgb_hsv_to_grb(h, cfg->side_sat, v);
  }
}

void rgb_effects_render(rgb_render_ctx_t const *ctx, rgb_grb_t out_leds[RGB_LED_COUNT])
{
  memset(out_leds, 0, RGB_LED_COUNT * sizeof(rgb_grb_t));
  if (ctx == NULL || ctx->cfg == NULL || ctx->fx == NULL) {
    return;
  }

  tk75_persist_cfg_t const *cfg = ctx->cfg;
  if (cfg->rgb_enabled != 0U && cfg->rgb_brightness != 0U && ctx->cal_done_mask != NULL) {
    /* Interactive Hall calibration RGB guide:
     * Blue = pending key, Bright Cyan-White = currently pressed, Mint Green = calibrated */
    uint8_t bright = cfg->rgb_brightness;
    for (uint8_t r = 0U; r < HALL_ROWS; r++) {
      for (uint8_t c = 0U; c < HALL_COLS; c++) {
        uint8_t led_idx = k_matrix_led_map[r][c];
        if (led_idx >= RGB_KEY_LED_COUNT) {
          continue;
        }
        uint8_t slot = (uint8_t)((r * HALL_COLS) + c);
        if (slot == ctx->cal_active_slot) {
          out_leds[led_idx] = rgb_hsv_to_grb(128U, 60U, bright);
        } else if (ctx->cal_done_mask[slot] != 0U) {
          out_leds[led_idx] = rgb_hsv_to_grb(96U, 255U, bright);
        } else {
          out_leds[led_idx] = rgb_hsv_to_grb(160U, 255U, bright);
        }
      }
    }
    render_sidelight(ctx, out_leds);
    return;
  }

  if (cfg->rgb_enabled != 0U && cfg->rgb_effect != RGB_FX_OFF && cfg->rgb_brightness != 0U) {
    rgb_frame_cache_t fc;
    fc.phase      = (uint8_t)(((ctx->now_ms * ((uint32_t)cfg->rgb_speed + 32U)) >> 9U) & 0xFFU);
    fc.layer_mask = (ctx->layer_mask != 0U) ? ctx->layer_mask : (1UL << ctx->active_layer);
    fc.fn_held    = ((fc.layer_mask & (1UL << 1U)) != 0U);
    fc.macro_pop_mask = fc.fn_held ? build_macro_populated_mask(cfg->macros) : 0U;
    if (cfg->rgb_effect == RGB_FX_SOLID) {
      fc.uniform_base = rgb_hsv_to_grb(cfg->rgb_hue, cfg->rgb_sat, cfg->rgb_brightness);
    } else if (cfg->rgb_effect == RGB_FX_BREATHING) {
      uint8_t v = scale8(triangle_wave8(fc.phase), cfg->rgb_brightness);
      fc.uniform_base = rgb_hsv_to_grb(cfg->rgb_hue, cfg->rgb_sat, v);
    } else {
      fc.uniform_base = (rgb_grb_t){ 0U, 0U, 0U };
    }
    uint8_t hl_h    = (uint8_t)(cfg->rgb_hue + 128U);
    uint8_t hl_s    = (cfg->rgb_sat >= 64U) ? cfg->rgb_sat : 200U;
    fc.macro_hl_col = rgb_hsv_to_grb(hl_h, hl_s, cfg->rgb_brightness);

    for (uint8_t r = 0U; r < HALL_ROWS; r++) {
      for (uint8_t c = 0U; c < HALL_COLS; c++) {
        uint8_t led_idx = k_matrix_led_map[r][c];
        if (led_idx < RGB_KEY_LED_COUNT) {
          out_leds[led_idx] = render_key_color(ctx, &fc, r, c, led_idx);
        }
      }
    }
  }

  /* Optional Caps Lock white/amber indicator on Caps Lock LED (#57 = row 3, col 0) */
  if (cfg->caps_ind_enabled != 0U && (ctx->host_leds & 0x02U) != 0U) {
    uint8_t ind_v = (cfg->rgb_brightness > 80U) ? cfg->rgb_brightness : 120U;
    out_leds[57] = (rgb_grb_t){ ind_v, ind_v, (uint8_t)(ind_v >> 1U) };
  }

  /* Win-Lock red indicator glow on LGUI LED (#80 = row 5, col 1) */
  if (cfg->win_lock != 0U) {
    out_leds[80] = (rgb_grb_t){ 0U, 180U, 0U };
  }

  render_sidelight(ctx, out_leds);
}
