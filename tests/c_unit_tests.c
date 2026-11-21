#include "encoder.h"
#include "factory_cal.h"
#include "flash_eeprom.h"
#include "hall_cal.h"
#include "hall_engine.h"
#include "hud_engine.h"
#include "pixel_hud.h"
#include "keymap.h"
#include "macro_player.h"
#include "macro_recorder.h"
#include "mode_switches.h"
#include "rgb_effects.h"
#include "spi3_rf.h"
#include "uart7_tft.h"
#include "via_protocol.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint32_t g_checks = 0U;

#define CHECK(cond)                                                        \
  do {                                                                     \
    if (!(cond)) {                                                         \
      fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
      return 1;                                                            \
    }                                                                      \
    g_checks++;                                                            \
  } while (0)

/* ---- In-memory NOR flash mock for EEPROM tests ---- */
typedef struct {
  uint8_t sectors[TK75_EEPROM_SLOTS][4096];
  bool    fail_write_once;
} mock_flash_t;

static bool mock_read(uint8_t slot, void *dst, uint32_t len, void *ctx)
{
  mock_flash_t const *mf = (mock_flash_t const *)ctx;
  memcpy(dst, mf->sectors[slot], len);
  return true;
}

static bool mock_erase(uint8_t slot, void *ctx)
{
  mock_flash_t *mf = (mock_flash_t *)ctx;
  memset(mf->sectors[slot], 0xFF, sizeof(mf->sectors[slot]));
  return true;
}

static bool mock_write(uint8_t slot, void const *src, uint32_t len, void *ctx)
{
  mock_flash_t *mf = (mock_flash_t *)ctx;
  memcpy(mf->sectors[slot], src, len);
  return true;
}

static int test_factory_cal(void)
{
  uint8_t rest_page[FACTORY_CAL_PAGE_BYTES];
  uint8_t span_page[FACTORY_CAL_PAGE_BYTES];
  uint8_t flags_page[FACTORY_CAL_PAGE_BYTES];
  memset(rest_page, 0xFF, sizeof(rest_page));
  memset(span_page, 0xFF, sizeof(span_page));
  memset(flags_page, 0xFF, sizeof(flags_page));

  /* Valid magics */
  rest_page[0x7FE]  = 0x55U;
  rest_page[0x7FF]  = 0xAAU;
  span_page[0x7FE]  = 0x55U;
  span_page[0x7FF]  = 0xAAU;
  flags_page[0x0FE] = 0x55U;
  flags_page[0x0FF] = 0xAAU;

  /* Stock column-major slot (col=2, row=3) -> stock index = 2*6 + 3 = 15 */
  uint32_t col_major = (2U * 6U) + 3U;
  rest_page[col_major * 2U]      = (uint8_t)(2800U & 0xFFU);
  rest_page[col_major * 2U + 1U] = (uint8_t)(2800U >> 8U);
  /* Bottom ADC = 1400 -> span = 2800 - 1400 = 1400 */
  span_page[col_major * 2U]      = (uint8_t)(1400U & 0xFFU);
  span_page[col_major * 2U + 1U] = (uint8_t)(1400U >> 8U);
  flags_page[col_major]          = 1U;

  factory_cal_data_t cal;
  factory_cal_parse(rest_page, span_page, flags_page, &cal);
  CHECK(cal.rest_valid && cal.span_valid && cal.flags_valid);

  uint32_t row_major = (3U * HALL_COLS) + 2U;
  CHECK(cal.rest_adc[row_major] == 2800U);
  CHECK(cal.span_adc[row_major] == 1400U);
  CHECK(cal.switch_flags[row_major] == 1U);

  /* Corrupt magic -> safe fallback defaults */
  rest_page[0x7FE] = 0x00U;
  factory_cal_parse(rest_page, span_page, flags_page, &cal);
  CHECK(!cal.rest_valid);
  CHECK(cal.rest_adc[row_major] == HALL_DEFAULT_REST_ADC);
  return 0;
}

static int test_flash_eeprom(void)
{
  mock_flash_t mf;
  memset(&mf, 0xFF, sizeof(mf));
  flash_eeprom_backend_t be = {
    .read_slot  = mock_read,
    .erase_slot = mock_erase,
    .write_slot = mock_write,
    .ctx        = &mf,
  };

  tk75_persist_cfg_t cfg;
  uint8_t slot = flash_eeprom_load(&be, &cfg);
  CHECK(slot == 0U);
  CHECK(cfg.magic == TK75_CFG_MAGIC);
  CHECK(cfg.hall_actuation_um == HALL_DEFAULT_ACT_UM);

  cfg.hall_actuation_um = 950U;
  CHECK(flash_eeprom_save(&be, &cfg, &slot));
  CHECK(slot == 1U);

  cfg.hall_actuation_um = 400U;
  CHECK(flash_eeprom_save(&be, &cfg, &slot));
  CHECK(slot == 2U);

  /* Corrupt slot 2 checksum to simulate brownout -> load must fall back cleanly to slot 1 */
  mf.sectors[2][20] ^= 0x5AU;
  tk75_persist_cfg_t recovered;
  uint8_t loaded_slot = flash_eeprom_load(&be, &recovered);
  CHECK(loaded_slot == 1U);
  CHECK(recovered.hall_actuation_um == 950U);

  CHECK(flash_eeprom_erase_all(&be));
  loaded_slot = flash_eeprom_load(&be, &recovered);
  CHECK(recovered.hall_actuation_um == HALL_DEFAULT_ACT_UM);

  /* Sequence wraparound across 0xFFFFFFFFU -> 0x00000000U */
  cfg.seq = 0xFFFFFFFEU;
  cfg.hall_actuation_um = 1100U;
  CHECK(flash_eeprom_save(&be, &cfg, &loaded_slot)); /* seq = 0xFFFFFFFFU in slot 1 */
  cfg.hall_actuation_um = 1200U;
  CHECK(flash_eeprom_save(&be, &cfg, &loaded_slot)); /* seq = 0x00000000U in slot 2 */
  loaded_slot = flash_eeprom_load(&be, &recovered);
  CHECK(loaded_slot == 2U && recovered.hall_actuation_um == 1200U);
  return 0;
}

static int test_hall_rt_and_socd(void)
{
  hall_engine_t eng;
  hall_engine_init(&eng, NULL, NULL);

  /* Verify unpopulated VBAT slot (row=5, col=4) stays ignored */
  CHECK(eng.active_mask[(5U * HALL_COLS) + 4U] == 0U);

  uint16_t frame[HALL_SLOT_COUNT];
  for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    frame[i] = HALL_DEFAULT_REST_ADC;
  }

  /* Fixed actuation on key A (3,1) */
  uint8_t slot_a = (uint8_t)((3U * HALL_COLS) + 1U);
  uint8_t slot_d = (uint8_t)((3U * HALL_COLS) + 3U);

  /* Half travel (~2.0mm -> ADC ~ 2400 - 550 = 1850) > 1.80mm default actuation */
  frame[slot_a] = 1850U;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng, frame);
    hall_engine_apply_socd(&eng);
  }
  CHECK(hall_engine_get_travel_um(&eng, 3U, 1U) >= 1950U);
  CHECK(hall_engine_is_pressed(&eng, 3U, 1U));

  /* Enable Rapid Trigger on key A (1.0mm act, rt_down=0.3mm, rt_up=0.3mm) */
  hall_key_config_t rt_cfg = {
    .actuation_um    = 1000U,
    .release_hyst_um = 200U,
    .rt_enabled      = true,
    .rt_continuous   = false,
    .rt_down_um      = 300U,
    .rt_up_um        = 300U,
  };
  hall_pos_t pos_a = { 3U, 1U };
  hall_pos_t pos_d = { 3U, 3U };
  hall_engine_set_key_config(&eng, pos_a, &rt_cfg);
  hall_engine_set_key_config(&eng, pos_d, &rt_cfg);

  /* Push A deep (3.6mm -> ADC ~ 2400 - 990 = 1410) */
  frame[slot_a] = 1410U;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng, frame);
  }
  uint16_t peak_um = hall_engine_get_travel_um(&eng, 3U, 1U);
  CHECK(peak_um > 3300U);

  /* Lift only 0.6mm (still ~3.0mm down, well below fixed 1.0mm!): RT must release! */
  frame[slot_a] = (uint16_t)(HALL_DEFAULT_REST_ADC - 780U); /* ~2.8mm */
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng, frame);
  }
  CHECK(!eng.states[slot_a].raw_pressed);

  /* Push down 0.5mm from ~2.8mm -> RT must re-actuate immediately! */
  frame[slot_a] = (uint16_t)(HALL_DEFAULT_REST_ADC - 950U); /* ~3.45mm */
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng, frame);
  }
  CHECK(eng.states[slot_a].raw_pressed);

  /* SOCD Last Input Priority between A and D */
  hall_socd_pair_t socd = { pos_a, pos_d, HALL_SOCD_LAST_PRIORITY };
  hall_engine_set_socd_pair(&eng, 0U, &socd);
  hall_engine_apply_socd(&eng);
  CHECK(hall_engine_is_pressed(&eng, 3U, 1U));
  CHECK(!hall_engine_is_pressed(&eng, 3U, 3U));

  /* Now press D while A remains held -> D wins, A cancels */
  frame[slot_d] = 1410U;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng, frame);
    hall_engine_apply_socd(&eng);
  }
  CHECK(!hall_engine_is_pressed(&eng, 3U, 1U));
  CHECK(hall_engine_is_pressed(&eng, 3U, 3U));

  /* Neutral mode cancels both */
  socd.mode = HALL_SOCD_NEUTRAL;
  hall_engine_set_socd_pair(&eng, 0U, &socd);
  hall_engine_apply_socd(&eng);
  CHECK(!hall_engine_is_pressed(&eng, 3U, 1U));
  CHECK(!hall_engine_is_pressed(&eng, 3U, 3U));

  /* Bottom noise floor (< 60um -> 0) and gentle bidirectional idle rail-droop re-zero */
  hall_engine_t eng2;
  hall_engine_init(&eng2, NULL, NULL);
  for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    frame[i] = HALL_DEFAULT_REST_ADC;
  }
  hall_engine_seed_idle_baseline(&eng2, frame);
  /* Tiny 10-count noise (~36um < 60um deadzone) must read clean 0um */
  frame[slot_a] = (uint16_t)(HALL_DEFAULT_REST_ADC - 10U);
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng2, frame);
  }
  CHECK(hall_engine_get_travel_um(&eng2, 3U, 1U) == 0U);
  /* Steady 25-count downward rail droop (~90um) gently re-zeros back to 0um over idle ticks */
  frame[slot_a] = (uint16_t)(HALL_DEFAULT_REST_ADC - 25U);
  for (int t = 0; t < 18000; t++) {
    hall_engine_process_frame(&eng2, frame);
  }
  CHECK(hall_engine_get_travel_um(&eng2, 3U, 1U) == 0U);

  /* Common-mode rail droop (USB unplug / RGB 0->100%: all 82 keys drop -10% = -240 counts)
   * must produce ZERO false travel and ZERO false actuation on Frame 1..20! */
  hall_engine_t eng3;
  hall_engine_init(&eng3, NULL, NULL);
  for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    frame[i] = HALL_DEFAULT_REST_ADC;
  }
  hall_engine_seed_idle_baseline(&eng3, frame);
  for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    frame[i] = 2160U; /* -10.0% supply rail droop on every switch simultaneously */
  }
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng3, frame);
    for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) {
      if (eng3.active_mask[i] != 0U) {
        CHECK(eng3.states[i].travel_um == 0U);
        CHECK(!eng3.states[i].logical_pressed);
      }
    }
  }
  CHECK(eng3.rail_scale_q12 >= 3670U && eng3.rail_scale_q12 <= 3705U); /* ~0.90x */

  /* Pressing key A halfway during the -10% droop must still actuate at exact ~2.0mm travel */
  frame[slot_a] = (uint16_t)(2160U - 495U); /* 0.9 * (2400 - 550) = 1665 */
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng3, frame);
  }
  CHECK(hall_engine_is_pressed(&eng3, 3U, 1U));
  CHECK(hall_engine_get_travel_um(&eng3, 3U, 1U) >= 1900U &&
        hall_engine_get_travel_um(&eng3, 3U, 1U) <= 2100U);
  CHECK(hall_engine_get_travel_um(&eng3, 3U, 3U) == 0U);

  /* Recovering back to 1.0x rail (USB replug / RGB off) keeps all idle keys cleanly at 0um */
  for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    frame[i] = HALL_DEFAULT_REST_ADC;
  }
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&eng3, frame);
  }
  CHECK(eng3.rail_scale_q12 >= 4080U && eng3.rail_scale_q12 <= 4112U);
  CHECK(!hall_engine_is_pressed(&eng3, 3U, 1U));
  CHECK(hall_engine_get_travel_um(&eng3, 3U, 1U) == 0U);
  return 0;
}

typedef struct {
  uint16_t codes[4];
  uint8_t  count;
} test_km_action_obs_t;

static void record_km_action(uint16_t kc, bool pressed, void *ctx)
{
  test_km_action_obs_t *obs = (test_km_action_obs_t *)ctx;
  if (pressed && obs != NULL && obs->count < 4U) {
    obs->codes[obs->count++] = kc;
  }
}

static int test_encoder_and_keymap_macros(void)
{
  /* Half-period EC11 quadrature: 2 Gray edges per detent (00<->11) + contact bounce recovery */
  encoder_state_t enc;
  encoder_state_init(&enc, false, false);
  uint8_t cw_seq[4] = { 1U, 3U, 2U, 0U };
  int8_t sum_cw = 0;
  for (int i = 0; i < 4; i++) {
    sum_cw = (int8_t)(sum_cw + encoder_state_step(&enc, cw_seq[i], false));
  }
  CHECK(sum_cw == 2);

  /* Contact bounce 00 -> 01 -> 00 -> 01 -> 11 yields cleanly 1 CW click */
  int8_t bounce_sum = 0;
  bounce_sum = (int8_t)(bounce_sum + encoder_state_step(&enc, 1U, false));
  bounce_sum = (int8_t)(bounce_sum + encoder_state_step(&enc, 0U, false));
  bounce_sum = (int8_t)(bounce_sum + encoder_state_step(&enc, 1U, false));
  bounce_sum = (int8_t)(bounce_sum + encoder_state_step(&enc, 3U, false));
  CHECK(bounce_sum == 1);

  /* Mid-detent chatter (11 -> 01 -> 11 -> 10 -> 11) and skipped 2-bit jump emit 0 spurious steps */
  int8_t chatter_sum = 0;
  chatter_sum = (int8_t)(chatter_sum + encoder_state_step(&enc, 1U, false));
  chatter_sum = (int8_t)(chatter_sum + encoder_state_step(&enc, 3U, false));
  chatter_sum = (int8_t)(chatter_sum + encoder_state_step(&enc, 2U, false));
  chatter_sum = (int8_t)(chatter_sum + encoder_state_step(&enc, 3U, false));
  chatter_sum = (int8_t)(chatter_sum + encoder_state_step(&enc, 0U, false));
  CHECK(chatter_sum == 0);

  /* Reset to 00 cleanly, then 2 CCW detents (11 <-> 00 via 2,3,1,0) */
  encoder_state_init(&enc, false, false);
  uint8_t ccw_seq[4] = { 2U, 3U, 1U, 0U };
  int8_t sum_ccw = 0;
  for (int i = 0; i < 4; i++) {
    sum_ccw = (int8_t)(sum_ccw + encoder_state_step(&enc, ccw_seq[i], true));
  }
  CHECK(sum_ccw == -2);
  CHECK(!enc.btn_state);
  /* 5 total ticks reaches debounce threshold */
  encoder_state_step(&enc, 0U, true);
  CHECK(enc.btn_state);

  /* ISO keymap + MO(1) latch safety + macro playback */
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);

  hall_engine_t hall;
  hall_engine_init(&hall, NULL, NULL);
  keymap_engine_t km;
  keymap_engine_init(&km, &cfg, &hall);

  /* Hold MO(1) @ (5, 10) + F1 @ (0, 1) -> Layer 1 puts KC_BRID (consumer 0x0070) */
  uint16_t frame[HALL_SLOT_COUNT];
  for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    frame[i] = HALL_DEFAULT_REST_ADC;
  }
  frame[(5U * HALL_COLS) + 10U] = 1400U;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&hall, frame);
    hall_engine_apply_socd(&hall);
  }
  keymap_report_t rep;
  keymap_build_report(&km, &rep);
  CHECK(rep.active_layer == 1U);

  /* Press Esc/Home @ (1,14) which on Layer 1 is KC_END (0x4D) */
  frame[(1U * HALL_COLS) + 14U] = 1400U;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&hall, frame);
    hall_engine_apply_socd(&hall);
  }
  keymap_build_report(&km, &rep);
  CHECK((rep.nkro_bitmap[KC_END >> 3U] & (1U << (KC_END & 7U))) != 0U);

  /* Release MO(1) while key stays held -> MUST remain KC_END (latched) and NOT get stuck! */
  frame[(5U * HALL_COLS) + 10U] = HALL_DEFAULT_REST_ADC;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&hall, frame);
    hall_engine_apply_socd(&hall);
  }
  keymap_build_report(&km, &rep);
  CHECK((rep.nkro_bitmap[KC_END >> 3U] & (1U << (KC_END & 7U))) != 0U);

  /* Macro test: string "Ab" + null terminator in slot 0 */
  memset(cfg.macros, 0, sizeof(cfg.macros));
  cfg.macros[0] = (uint8_t)'A';
  cfg.macros[1] = (uint8_t)'b';
  cfg.macros[2] = 0U;
  macro_player_t mp;
  macro_player_init(&mp);
  macro_player_trigger(&mp, cfg.macros, 0U, 100U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 100U, &rep);
  CHECK((rep.modifiers & 0x02U) != 0U); /* Left Shift for uppercase A */
  CHECK((rep.nkro_bitmap[KC_A >> 3U] & (1U << (KC_A & 7U))) != 0U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 110U, &rep); /* Release tap A after 10ms hold */
  CHECK(rep.modifiers == 0U);
  CHECK((rep.nkro_bitmap[KC_A >> 3U] & (1U << (KC_A & 7U))) == 0U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 118U, &rep); /* Emit 'b' after 8ms release gap */
  CHECK((rep.nkro_bitmap[KC_B >> 3U] & (1U << (KC_B & 7U))) != 0U);

  /* Encoder CW (+1) emits Volume Up (0x00E9) and CCW (-1) emits Volume Down (0x00EA) */
  keymap_push_encoder(&km, sum_cw, false);
  keymap_build_report(&km, &rep);
  CHECK(rep.consumer_usage == 0x00E9U);
  keymap_push_encoder(&km, sum_ccw, false);
  keymap_build_report(&km, &rep);
  CHECK(rep.consumer_usage == 0x00EAU);

  /* Overlay action on (2,15) must NOT trigger on base Layer 0, MUST trigger with Fn (Layer 1) even in Mac mode */
  test_km_action_obs_t km_obs;
  memset(&km_obs, 0, sizeof(km_obs));
  keymap_set_action_cb(&km, record_km_action, &km_obs);
  keymap_set_mac_mode(&km, true);
  CHECK(!keymap_try_encoder_overlay_action(&km));
  CHECK(km_obs.count == 0U);

  /* Hold MO(1) while Mac mode (Layer 2) active: Fn+Knob turn fires RGB_VAI, Fn+Knob push fires KC_ENC_MODE_NEXT */
  frame[(5U * HALL_COLS) + 10U] = 1400U;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&hall, frame);
    hall_engine_apply_socd(&hall);
  }
  keymap_build_report(&km, &rep);
  CHECK(km_obs.count == 1U && km_obs.codes[0] == MO(1));
  memset(&km_obs, 0, sizeof(km_obs));
  CHECK(keymap_try_encoder_overlay_action(&km));
  CHECK(km_obs.count == 1U && km_obs.codes[0] == KC_ENC_MODE_NEXT);
  keymap_push_encoder(&km, 2, false);
  CHECK(km_obs.count == 3U && km_obs.codes[1] == RGB_VAI && km_obs.codes[2] == RGB_VAI);

  /* Held MUTE push + reset_encoder_state prevents stuck consumer code when leaving encoder mode 0 */
  keymap_set_mac_mode(&km, false);
  frame[(5U * HALL_COLS) + 10U] = HALL_DEFAULT_REST_ADC;
  for (int t = 0; t < 20; t++) {
    hall_engine_process_frame(&hall, frame);
    hall_engine_apply_socd(&hall);
  }
  keymap_build_report(&km, &rep);
  keymap_push_encoder(&km, 1, true);
  keymap_reset_encoder_state(&km);
  keymap_build_report(&km, &rep);
  CHECK(rep.consumer_usage == 0U);
  return 0;
}

typedef struct {
  via_event_type_t last_evt;
  via_rtc_time_t   rtc;
  uint16_t         media_chunks;
} test_via_obs_t;

static void via_evt_cb(via_event_t const *evt, void *ctx)
{
  test_via_obs_t *obs = (test_via_obs_t *)ctx;
  obs->last_evt = evt->type;
  if (evt->type == VIA_EVT_RTC_SYNC) {
    obs->rtc = evt->rtc;
  } else if (evt->type == VIA_EVT_TFT_MEDIA_CHUNK) {
    obs->media_chunks++;
  }
}

static int test_via_and_rgb(void)
{
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);
  hall_engine_t hall;
  hall_engine_init(&hall, NULL, NULL);

  test_via_obs_t obs;
  memset(&obs, 0, sizeof(obs));
  via_context_t via = {
    .cfg       = &cfg,
    .hall      = &hall,
    .uptime_ms = 0x12345678UL,
    .on_event  = via_evt_cb,
    .event_ctx = &obs,
  };

  uint8_t req[32];
  uint8_t rep[32];

  /* Protocol version */
  memset(req, 0, sizeof(req));
  req[0] = 0x01U;
  via_process_packet(&via, req, rep);
  CHECK(rep[1] == 0x00U && rep[2] == 0x0CU);

  /* Hall actuation set/get (ch=5, val_id=1 -> step 12 = 600um) */
  memset(req, 0, sizeof(req));
  req[0] = 0x07U;
  req[1] = 5U;
  req[2] = 1U;
  req[3] = 12U;
  via_process_packet(&via, req, rep);
  CHECK(cfg.hall_actuation_um == 600U);
  CHECK(obs.last_evt == VIA_EVT_HALL_CONFIG_CHANGED);

  memset(req, 0, sizeof(req));
  req[0] = 0x08U;
  req[1] = 5U;
  req[2] = 1U;
  via_process_packet(&via, req, rep);
  CHECK(rep[3] == 12U);

  /* Ext RTC sync (ch=8, val_id=2) */
  memset(req, 0, sizeof(req));
  req[0] = 0x07U;
  req[1] = 8U;
  req[2] = 2U;
  req[3] = 0x07U; /* 2026 = 0x07EA */
  req[4] = 0xEAU;
  req[5] = 11U;
  req[6] = 15U;
  req[7] = 14U;
  req[8] = 30U;
  req[9] = 45U;
  via_process_packet(&via, req, rep);
  CHECK(obs.last_evt == VIA_EVT_RTC_SYNC);
  CHECK(obs.rtc.year == 2026U && obs.rtc.month == 11U && obs.rtc.sec == 45U);

  /* GET_KEYBOARD_VALUE id_firmware_version (0x04) */
  memset(req, 0, sizeof(req));
  req[0] = 0x02U;
  req[1] = 0x04U;
  via_process_packet(&via, req, rep);
  CHECK(rep[0] == 0x02U && rep[1] == 0x04U);
  CHECK(rep[2] == 0x00U && rep[3] == 0x01U && rep[4] == 0x00U && rep[5] == 0x00U);

  /* Macro buffer trailing 28-byte slice when buffer size is not divisible by 28 */
  uint16_t const tail_off = (uint16_t)((TK75_MACRO_BUF_BYTES / 28U) * 28U);
  uint16_t const tail_rem = (uint16_t)(TK75_MACRO_BUF_BYTES - tail_off);
  CHECK(tail_rem > 0U && tail_rem < 28U);
  memset(req, 0xAA, sizeof(req));
  req[0] = 0x0FU; /* ID_DYNAMIC_KEYMAP_MACRO_SET_BUF */
  req[1] = (uint8_t)(tail_off >> 8U);
  req[2] = (uint8_t)(tail_off & 0xFFU);
  req[3] = 28U;
  for (uint8_t i = 0U; i < 28U; i++) {
    req[4U + i] = (uint8_t)(0x40U + i);
  }
  via_process_packet(&via, req, rep);
  CHECK(rep[0] == 0x0FU);
  CHECK(obs.last_evt == VIA_EVT_SAVE_EEPROM);

  memset(req, 0, sizeof(req));
  req[0] = 0x0EU; /* ID_DYNAMIC_KEYMAP_MACRO_GET_BUF */
  req[1] = (uint8_t)(tail_off >> 8U);
  req[2] = (uint8_t)(tail_off & 0xFFU);
  req[3] = 28U;
  via_process_packet(&via, req, rep);
  CHECK(rep[0] == 0x0EU);
  CHECK(rep[4] == 0x40U && rep[4U + tail_rem - 1U] == (uint8_t)(0x40U + tail_rem - 1U));
  CHECK(rep[4U + tail_rem] == 0x00U && rep[31] == 0x00U); /* Clamped remainder zero-padded */

  req[1] = (uint8_t)(TK75_MACRO_BUF_BYTES >> 8U);
  req[2] = (uint8_t)(TK75_MACRO_BUF_BYTES & 0xFFU);
  req[3] = 1U;
  via_process_packet(&via, req, rep);
  CHECK(rep[0] == 0xFFU);

  /* Custom menu GET sweep matching via/tk75hev2_optionb_via.json */
  static struct {
    uint8_t ch;
    uint8_t max_id;
  } const via_menus[] = {
    { 3U, 4U },
    { 5U, 9U },
    { 6U, 7U },
    { 7U, 8U },
    { 9U, 5U },
  };
  for (size_t m = 0U; m < sizeof(via_menus) / sizeof(via_menus[0]); m++) {
    for (uint8_t vid = 1U; vid <= via_menus[m].max_id; vid++) {
      memset(req, 0, sizeof(req));
      req[0] = 0x08U;
      req[1] = via_menus[m].ch;
      req[2] = vid;
      via_process_packet(&via, req, rep);
      CHECK(rep[0] == 0x08U && rep[1] == via_menus[m].ch && rep[2] == vid);
    }
  }

  /* RGB render + Caps indicator override */
  rgb_effects_state_t fx;
  memset(&fx, 0, sizeof(fx));
  rgb_grb_t leds[RGB_LED_COUNT];
  rgb_render_ctx_t rctx = {
    .fx           = &fx,
    .cfg          = &cfg,
    .hall         = &hall,
    .active_layer = 0U,
    .host_leds    = 0x02U, /* Caps Lock active */
    .now_ms       = 100U,
  };
  cfg.rgb_effect       = RGB_FX_SOLID;
  cfg.rgb_hue          = 0U;
  cfg.rgb_sat          = 255U;
  cfg.rgb_brightness   = 200U;
  cfg.caps_ind_enabled = 1U;
  rgb_effects_render(&rctx, leds);
  uint8_t caps_idx = rgb_matrix_to_led_idx(3U, 0U);
  CHECK(caps_idx == 57U && caps_idx < RGB_KEY_LED_COUNT);
  CHECK(leds[caps_idx].r == 200U && leds[caps_idx].g == 200U && leds[caps_idx].b == 100U);

  /* Per-layer highlight: L1 (Fn) uses equilateral triadic contrast (+85 Fn, +171 set macros)
   * on colored base + dims unbound keys to 25%, guaranteeing high contrast for every base color. */
  rctx.host_leds    = 0U;
  rctx.active_layer = 1U;
  rctx.layer_mask   = (1UL << 1U) | 1UL;
  uint8_t const m0_sample[3] = { 0x01U, 0x01U, (uint8_t)KC_A };
  CHECK(macro_recorder_save_slot(cfg.macros, 0U, m0_sample, 3U));
  rgb_effects_render(&rctx, leds);
  rgb_grb_t base_col = rgb_hsv_to_grb(0U, 255U, 200U);
  rgb_grb_t l1_fn    = rgb_hsv_to_grb(85U, 255U, 200U);
  rgb_grb_t l1_macro = rgb_hsv_to_grb(171U, 255U, 200U);
  uint8_t   fn_q_idx = rgb_matrix_to_led_idx(2U, 1U);  /* Q -> KC_BT1 on L1 */
  uint8_t   fn_c_idx = rgb_matrix_to_led_idx(4U, 4U);  /* C -> MACRO(0) on L1 */
  uint8_t   fn_k_idx = rgb_matrix_to_led_idx(5U, 10U); /* MO(1) key itself */
  uint8_t   trns_idx = rgb_matrix_to_led_idx(3U, 1U);  /* A -> KC_TRNS on L1 */
  CHECK(leds[fn_q_idx].r == l1_fn.r && leds[fn_q_idx].g == l1_fn.g && leds[fn_q_idx].b == l1_fn.b);
  CHECK(leds[fn_k_idx].r == l1_fn.r && leds[fn_k_idx].g == l1_fn.g && leds[fn_k_idx].b == l1_fn.b);
  CHECK(leds[fn_c_idx].r == l1_macro.r && leds[fn_c_idx].g == l1_macro.g && leds[fn_c_idx].b == l1_macro.b);
  CHECK(leds[trns_idx].r == (base_col.r >> 2U) && leds[trns_idx].g == (base_col.g >> 2U) &&
        leds[trns_idx].b == (base_col.b >> 2U));

  /* White/pastel base (sat=0) uses Vivid Orange for Fn & Emerald Green for populated macros */
  cfg.rgb_sat = 0U;
  rgb_effects_render(&rctx, leds);
  rgb_grb_t w_fn    = rgb_hsv_to_grb(20U, 255U, 200U);
  rgb_grb_t w_macro = rgb_hsv_to_grb(85U, 255U, 200U);
  CHECK(leds[fn_q_idx].r == w_fn.r && leds[fn_q_idx].g == w_fn.g && leds[fn_q_idx].b == w_fn.b);
  CHECK(leds[fn_c_idx].r == w_macro.r && leds[fn_c_idx].g == w_macro.g && leds[fn_c_idx].b == w_macro.b);
  CHECK(leds[trns_idx].r == 50U && leds[trns_idx].g == 50U && leds[trns_idx].b == 50U);

  /* Full 360-deg hue sweep: Fn, populated Macro, and base color stay strongly distinct everywhere */
  cfg.rgb_sat = 255U;
  for (uint16_t h = 0U; h < 256U; h += 16U) {
    cfg.rgb_hue = (uint8_t)h;
    rgb_effects_render(&rctx, leds);
    rgb_grb_t bc = rgb_hsv_to_grb((uint8_t)h, 255U, 200U);
    rgb_grb_t fc = leds[fn_q_idx];
    rgb_grb_t mc = leds[fn_c_idx];
    int32_t d_bf = (fc.r > bc.r ? fc.r - bc.r : bc.r - fc.r) +
                   (fc.g > bc.g ? fc.g - bc.g : bc.g - fc.g) +
                   (fc.b > bc.b ? fc.b - bc.b : bc.b - fc.b);
    int32_t d_bm = (mc.r > bc.r ? mc.r - bc.r : bc.r - mc.r) +
                   (mc.g > bc.g ? mc.g - bc.g : bc.g - mc.g) +
                   (mc.b > bc.b ? mc.b - bc.b : bc.b - mc.b);
    int32_t d_fm = (fc.r > mc.r ? fc.r - mc.r : mc.r - fc.r) +
                   (fc.g > mc.g ? fc.g - mc.g : mc.g - fc.g) +
                   (fc.b > mc.b ? fc.b - mc.b : mc.b - fc.b);
    CHECK(d_bf >= 250 && d_bm >= 250 && d_fm >= 250);
  }
  cfg.rgb_hue = 0U;

  /* L2 (macOS) uses exact 180 deg complement (+128), L3 (Custom) uses triadic complement (+85) */
  cfg.keymap[3][3][1] = KC_SPC; /* Bind 'A' on L3 */
  rctx.active_layer   = 3U;
  rctx.layer_mask     = (1UL << 3U) | (1UL << 2U) | (1UL << 1U) | 1UL;
  rgb_effects_render(&rctx, leds);
  rgb_grb_t l2_comp = rgb_hsv_to_grb(128U, 255U, 200U);
  rgb_grb_t l3_comp = rgb_hsv_to_grb(85U, 255U, 200U);
  uint8_t   l2_idx  = rgb_matrix_to_led_idx(5U, 1U); /* LGUI->LALT on L2 */
  CHECK(leds[fn_q_idx].r == l1_fn.r && leds[fn_q_idx].g == l1_fn.g && leds[fn_q_idx].b == l1_fn.b);
  CHECK(leds[l2_idx].r == l2_comp.r && leds[l2_idx].g == l2_comp.g && leds[l2_idx].b == l2_comp.b);
  CHECK(leds[trns_idx].r == l3_comp.r && leds[trns_idx].g == l3_comp.g && leds[trns_idx].b == l3_comp.b);

  /* Active macro recording blinks target macro key + Fn key red */
  rctx.macro_rec_active = true;
  rctx.macro_rec_slot   = 0U;
  rctx.now_ms           = 128U; /* blink_on */
  rgb_effects_render(&rctx, leds);
  rgb_grb_t rec_red = rgb_hsv_to_grb(0U, 255U, 200U);
  CHECK(leds[fn_c_idx].r == rec_red.r && leds[fn_c_idx].g == rec_red.g && leds[fn_c_idx].b == rec_red.b);
  CHECK(leds[fn_k_idx].r == rec_red.r && leds[fn_k_idx].g == rec_red.g && leds[fn_k_idx].b == rec_red.b);
  rctx.now_ms = 0U; /* blink_off */
  rgb_effects_render(&rctx, leds);
  CHECK(leds[fn_c_idx].r == 0U && leds[fn_c_idx].g == 0U && leds[fn_c_idx].b == 0U);
  rctx.macro_rec_active = false;

  /* Brightness == 0 or RGB OFF suppresses layer illumination completely */
  cfg.rgb_brightness = 0U;
  rgb_effects_render(&rctx, leds);
  CHECK(leds[fn_q_idx].r == 0U && leds[fn_q_idx].g == 0U && leds[fn_q_idx].b == 0U);
  CHECK(leds[l2_idx].r == 0U && leds[l2_idx].g == 0U && leds[l2_idx].b == 0U);
  CHECK(leds[trns_idx].r == 0U && leds[trns_idx].g == 0U && leds[trns_idx].b == 0U);
  cfg.rgb_brightness = 200U;
  cfg.rgb_enabled    = 0U;
  rgb_effects_render(&rctx, leds);
  CHECK(leds[fn_q_idx].r == 0U && leds[fn_q_idx].g == 0U && leds[fn_q_idx].b == 0U);
  return 0;
}

typedef struct {
  uint8_t  cmds[8];
  uint16_t lens[8];
  uint8_t  last_payload[UART7_MAX_PAYLOAD];
  uint8_t  count;
} tx_rec_t;

static bool record_tx(uart7_tx_frame_t const *frame, void *ctx)
{
  tx_rec_t *rec = (tx_rec_t *)ctx;
  if (rec->count < 8U) {
    rec->cmds[rec->count] = frame->cmd;
    rec->lens[rec->count] = frame->payload_len;
    rec->count++;
  }
  if (frame->payload_len > 0U && frame->payload_len <= sizeof(rec->last_payload)) {
    memcpy(rec->last_payload, frame->payload, frame->payload_len);
  }
  return true;
}

static int test_uart7_and_hud(void)
{
  /* TX wire format matching stock app.c (0x55 <len_u16_le=plen+2> <cmd> <payload> <sum8>) */
  uint8_t ping_wire[8];
  uart7_tx_frame_t ping = { .cmd = 0x3EU, .payload_len = 0U, .payload = NULL };
  uint16_t wlen = uart7_tft_encode_frame(&ping, ping_wire, sizeof(ping_wire));
  CHECK(wlen == 5U);
  CHECK(ping_wire[0] == 0x55U);
  CHECK(ping_wire[1] == 0x02U && ping_wire[2] == 0x00U);
  CHECK(ping_wire[3] == 0x3EU);
  CHECK(ping_wire[4] == (uint8_t)(0x02U + 0x00U + 0x3EU));

  /* RX 'D' parser with noise prefix */
  uart7_rx_parser_t parser;
  uart7_tft_rx_init(&parser);
  uint8_t rx_stream[10] = {
    0xFF, 0x00,             /* junk noise */
    'D', 5U, 0x82U,         /* sync, len=5, cmd=0x82 */
    1U, 3U, 0U,             /* payload (3 B = len-2) */
    (uint8_t)(5U + 0x82U + 1U + 3U + 0U)
  };
  uart7_rx_frame_t rx;
  bool got = false;
  for (size_t i = 0U; i < sizeof(rx_stream); i++) {
    if (uart7_tft_rx_feed(&parser, rx_stream[i], &rx)) {
      got = true;
    }
  }
  CHECK(got);
  CHECK(rx.cmd == 0x82U && rx.payload_len == 3U && rx.payload[0] == 1U && rx.payload[1] == 3U);

  /* HUD engine RTC rollover & media 63-byte chunk flush */
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);
  tx_rec_t rec;
  memset(&rec, 0, sizeof(rec));
  hud_engine_t hud;
  hud_init_cfg_t hinit = { .cfg = &cfg, .send_fn = record_tx, .send_ctx = &rec };
  hud_engine_init(&hud, &hinit);

  via_rtc_time_t leap_eve = { 2024U, 2U, 28U, 23U, 59U, 59U };
  hud_engine_set_rtc(&hud, &leap_eve);
  hud_engine_tick(&hud, 5000U);
  CHECK(rec.count == 1U && rec.cmds[0] == HUD_CMD_RTC);
  hud_engine_tick(&hud, 6000U); /* Advances 1s -> 2024-02-29 00:00:00 */
  CHECK(hud.rtc.month == 2U && hud.rtc.day == 29U && hud.rtc.hour == 0U && hud.rtc.sec == 0U);

  via_media_chunk_t chunk = { .flags = 0x13U, .seq = 0U, .len = 16U };
  memset(chunk.data, 0xAB, 16U);
  CHECK(hud_engine_push_media_chunk(&hud, &chunk));
  CHECK(rec.cmds[rec.count - 2U] == HUD_CMD_MEDIA_PREP);
  CHECK(rec.cmds[rec.count - 1U] == HUD_CMD_MEDIA);
  CHECK(rec.last_payload[0] == 1U && rec.last_payload[6] == 16U && rec.last_payload[8] == 0xABU);

  /* Multi-chunk boundary test: 3x24B (72B) with explicit PREP must never drop bytes across 56B boundary */
  memset(&rec, 0, sizeof(rec));
  via_media_chunk_t prep = { .flags = 0x21U, .seq = 0xFFFFU, .len = 10U };
  prep.data[0] = 2U; /* frame_idx */
  prep.data[1] = 1U; /* total_frames */
  prep.data[3] = 72U;
  prep.data[7] = 128U;
  prep.data[9] = 128U;
  CHECK(hud_engine_push_media_chunk(&hud, &prep));
  CHECK(rec.count == 1U && rec.cmds[0] == HUD_CMD_MEDIA_PREP);

  for (uint8_t s = 0U; s < 3U; s++) {
    via_media_chunk_t dc = { .flags = (uint8_t)(0x20U | (s == 2U ? 0x02U : 0x00U)), .seq = s, .len = 24U };
    memset(dc.data, (int)(0x30U + s), 24U);
    CHECK(hud_engine_push_media_chunk(&hud, &dc));
  }
  /* Expect 1 PREP + 2 MEDIA frames (56B chunk 0 + 16B chunk 1 = 72B total) */
  CHECK(rec.count == 3U && rec.cmds[1] == HUD_CMD_MEDIA && rec.cmds[2] == HUD_CMD_MEDIA);
  CHECK(rec.last_payload[0] == 2U && rec.last_payload[4] == 1U && rec.last_payload[6] == 16U &&
        rec.last_payload[8] == 0x32U && rec.last_payload[23] == 0x32U);

  via_media_chunk_t erase = { .flags = 0x04U, .seq = 0U, .len = 0U };
  CHECK(hud_engine_push_media_chunk(&hud, &erase));
  CHECK(rec.cmds[rec.count - 1U] == HUD_CMD_MEDIA_ERASE && rec.lens[rec.count - 1U] == 0U);
  return 0;
}

typedef struct {
  uint8_t count;
  uint8_t last_len;
  uint8_t last_pkt[64];
} test_rf_via_obs_t;

static void rf_via_obs_cb(uint8_t const *pkt64, uint8_t len, void *ctx)
{
  test_rf_via_obs_t *obs = (test_rf_via_obs_t *)ctx;
  obs->count++;
  obs->last_len = len;
  if (len <= sizeof(obs->last_pkt)) {
    memcpy(obs->last_pkt, pkt64, len);
  }
}

static uint8_t test_sum8(uint8_t const *buf, uint8_t count)
{
  uint8_t s = 0U;
  for (uint8_t i = 0U; i < count; i++) {
    s = (uint8_t)(s + buf[i]);
  }
  return s;
}

static int test_spi3_rf_and_wireless(void)
{
  /* 1. Verified PB2/PB10 slider + PC13 VBUS + PC14 STAT decoder */
  mode_switch_state_t sw_bt = mode_switches_decode(false, true, true, true);
  CHECK(sw_bt.conn_mode == TK75_CONN_BT && !sw_bt.vbus_present && !sw_bt.charging);

  mode_switch_state_t sw_24g = mode_switches_decode(true, false, false, true);
  CHECK(sw_24g.conn_mode == TK75_CONN_24G && sw_24g.vbus_present && sw_24g.charging);

  mode_switch_state_t sw_usb_full = mode_switches_decode(true, true, false, false);
  CHECK(sw_usb_full.conn_mode == TK75_CONN_USB && sw_usb_full.vbus_present && !sw_usb_full.charging);

  mode_switch_state_t sw_both_low = mode_switches_decode(false, false, true, false);
  CHECK(sw_both_low.conn_mode == TK75_CONN_USB && !sw_both_low.vbus_present);

  /* 2. SPI3 RF TX frame encoding, checksum, and 4-byte DMA alignment */
  uint8_t buf[SPI3_RF_BUF_SIZE];
  uint16_t n = spi3_rf_build_poll(buf, (uint16_t)sizeof(buf));
  CHECK(n == 68U && buf[0] == SPI3_RF_CMD_POLL && buf[1] == 0U && buf[0x42] == 0U);

  uint8_t keys6[6] = { KC_A, KC_B, 0U, 0U, 0U, 0U };
  n = spi3_rf_build_boot_kbd(0x02U, keys6, buf, (uint16_t)sizeof(buf));
  CHECK(SPI3_RF_HID_MOUSE == 0U && SPI3_RF_HID_BOOT_KBD == 1U && SPI3_RF_HID_NKRO == 2U);
  CHECK(n == 12U && buf[0] == SPI3_RF_CMD_HID && buf[1] == 8U && buf[2] == 1U);
  CHECK(buf[3] == 0x02U && buf[4] == KC_A && buf[5] == KC_B);
  CHECK(buf[10] == test_sum8(&buf[2], 8U));

  uint8_t nkro15[15] = { 0 };
  nkro15[0] = 0x10U; /* KC_A (0x04) */
  nkro15[14] = 0x80U; /* KC 0x77 */
  n = spi3_rf_build_nkro_kbd(nkro15, buf, (uint16_t)sizeof(buf));
  CHECK(n == 20U && buf[0] == SPI3_RF_CMD_HID && buf[1] == 16U && buf[2] == SPI3_RF_HID_NKRO);
  CHECK(buf[3] == 0x10U && buf[17] == 0x80U && buf[18] == test_sum8(&buf[2], 16U));

  n = spi3_rf_build_consumer(0x00E9U, buf, (uint16_t)sizeof(buf));
  CHECK(n == 8U && buf[1] == 3U && buf[2] == SPI3_RF_HID_CONSUMER && buf[3] == 0xE9U && buf[4] == 0x00U);
  CHECK(buf[5] == test_sum8(&buf[2], 3U));

  n = spi3_rf_build_system(0x82U, buf, (uint16_t)sizeof(buf));
  CHECK(n == 8U && buf[1] == 2U && buf[2] == SPI3_RF_HID_SYSTEM && buf[3] == 0x82U);
  CHECK(buf[4] == test_sum8(&buf[2], 2U));

  uint8_t via_tx[32];
  memset(via_tx, 0, sizeof(via_tx));
  via_tx[0] = 0x01U;
  via_tx[5] = 0x5AU;
  n = spi3_rf_build_via(via_tx, 32U, buf, (uint16_t)sizeof(buf));
  CHECK(n == 68U && buf[1] == 65U && buf[2] == SPI3_RF_HID_VIA && buf[3] == 0x01U && buf[8] == 0x5AU);
  CHECK(buf[67] == test_sum8(&buf[2], 65U));

  n = spi3_rf_build_ble_name("TK75 VIA", buf, (uint16_t)sizeof(buf));
  CHECK(n == 36U && buf[0] == SPI3_RF_CMD_PAIR_NAME && buf[1] == 33U && buf[2] == 2U && buf[3] == 8U);
  CHECK(memcmp(&buf[4], "TK75 VIA", 8U) == 0 && buf[35] == test_sum8(&buf[2], 33U));

  n = spi3_rf_build_set_mode(SPI3_RF_MODE_BT2, buf, (uint16_t)sizeof(buf));
  CHECK(n == 4U && buf[0] == SPI3_RF_CMD_SET_MODE && buf[1] == 1U && buf[2] == 1U && buf[3] == 1U);

  n = spi3_rf_build_pair(3U, buf, (uint16_t)sizeof(buf));
  CHECK(n == 4U && buf[0] == SPI3_RF_CMD_PAIR_NAME && buf[2] == 3U && buf[3] == 3U);

  n = spi3_rf_build_poll_rate(2U, buf, (uint16_t)sizeof(buf));
  CHECK(n == 4U && buf[0] == SPI3_RF_CMD_POLL_RATE && buf[2] == 2U && buf[3] == 2U);

  n = spi3_rf_build_battery(77U, buf, (uint16_t)sizeof(buf));
  CHECK(n == 4U && buf[0] == SPI3_RF_CMD_BATTERY && buf[2] == 77U && buf[3] == 77U);

  n = spi3_rf_build_sleep(buf, (uint16_t)sizeof(buf));
  CHECK(n == 8U && buf[0] == SPI3_RF_CMD_SLEEP && buf[1] == 2U && buf[2] == 0U && buf[3] == 0U && buf[4] == 0U);

  /* 3. SPI3 RF RX decode, CRC failure, busy handling, mode auto-resync & VIA RX */
  spi3_rf_state_t rf;
  spi3_rf_state_init(&rf, SPI3_RF_MODE_BT2, 1U);
  CHECK(rf.target_mode == SPI3_RF_MODE_BT2 && rf.poll_rate_idx == 1U && rf.mode_sync_needed);

  uint8_t rx_buf[SPI3_RF_BUF_SIZE];
  memset(rx_buf, 0, sizeof(rx_buf));
  rx_buf[0] = 0xFFU;
  rx_buf[1] = 4U;
  rx_buf[2] = SPI3_RF_RX_STATUS;
  rx_buf[3] = 0x42U; /* Connected (0x40) + Caps Lock LED (0x02) */
  rx_buf[4] = SPI3_RF_STATE_CONNECTED;
  rx_buf[5] = SPI3_RF_MODE_24G; /* Mismatched mode triggers mode_sync_needed */
  rx_buf[6] = test_sum8(&rx_buf[2], 4U);

  spi3_rf_rx_frame_t rxf;
  CHECK(spi3_rf_decode_rx(rx_buf, (uint16_t)sizeof(rx_buf), &rxf) == SPI3_RF_RX_OK);
  test_rf_via_obs_t rf_via_obs = { 0 };
  rf.link_up          = false;
  rf.batt_sync_needed = false;
  spi3_rf_apply_rx(&rf, &rxf, 100U, rf_via_obs_cb, &rf_via_obs);
  CHECK(rf.link_up && rf.host_leds == 0x02U && rf.reported_mode == SPI3_RF_MODE_24G);
  CHECK(rf.mode_sync_needed && rf.batt_sync_needed);

  rx_buf[5] = SPI3_RF_MODE_BT2;
  rx_buf[6] = test_sum8(&rx_buf[2], 4U);
  CHECK(spi3_rf_decode_rx(rx_buf, (uint16_t)sizeof(rx_buf), &rxf) == SPI3_RF_RX_OK);
  spi3_rf_apply_rx(&rf, &rxf, 120U, rf_via_obs_cb, &rf_via_obs);
  CHECK(!rf.mode_sync_needed);

  /* Corrupt CRC -> rejected */
  rx_buf[6] ^= 0x01U;
  CHECK(spi3_rf_decode_rx(rx_buf, (uint16_t)sizeof(rx_buf), &rxf) == SPI3_RF_RX_BAD_CRC);

  /* Busy status */
  memset(rx_buf, 0, sizeof(rx_buf));
  rx_buf[1] = 1U;
  rx_buf[2] = SPI3_RF_RX_BUSY1;
  rx_buf[3] = SPI3_RF_RX_BUSY1;
  CHECK(spi3_rf_decode_rx(rx_buf, (uint16_t)sizeof(rx_buf), &rxf) == SPI3_RF_RX_BUSY);

  /* Wireless VIA RX packet (0x00, 64B payload) */
  memset(rx_buf, 0, sizeof(rx_buf));
  rx_buf[1]  = 65U;
  rx_buf[2]  = SPI3_RF_RX_VENDOR;
  rx_buf[3]  = 0x07U;
  rx_buf[4]  = 0x09U;
  rx_buf[34] = 0x33U;
  rx_buf[67] = test_sum8(&rx_buf[2], 65U);
  CHECK(spi3_rf_decode_rx(rx_buf, (uint16_t)sizeof(rx_buf), &rxf) == SPI3_RF_RX_OK);
  spi3_rf_apply_rx(&rf, &rxf, 150U, rf_via_obs_cb, &rf_via_obs);
  CHECK(rf_via_obs.count == 1U && rf_via_obs.last_len == 64U &&
        rf_via_obs.last_pkt[0] == 0x07U && rf_via_obs.last_pkt[31] == 0x33U);

  /* Pair ack */
  memset(rx_buf, 0, sizeof(rx_buf));
  rx_buf[1] = 2U;
  rx_buf[2] = SPI3_RF_RX_PAIR_ACK;
  rx_buf[3] = 1U;
  rx_buf[4] = test_sum8(&rx_buf[2], 2U);
  rf.pair_pending = true;
  CHECK(spi3_rf_decode_rx(rx_buf, (uint16_t)sizeof(rx_buf), &rxf) == SPI3_RF_RX_OK);
  spi3_rf_apply_rx(&rf, &rxf, 200U, NULL, NULL);
  CHECK(rf.paired && !rf.pair_pending);

  /* 4. VIA Channel 9 (Wireless & Power) + Stage 2 keymap defaults & Mac swap */
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);
  CHECK(cfg.sleep_timeout_min == 5U && cfg.rf_bt_slot == 0U && cfg.mac_mode == 0U);
  CHECK(cfg.keymap[1][2][1] == KC_BT1 && cfg.keymap[1][2][4] == KC_RF_24G &&
        cfg.keymap[1][2][5] == KC_RF_PAIR && cfg.keymap[1][2][6] == KC_MAC_TOG);

  hall_engine_t hall;
  hall_engine_init(&hall, NULL, NULL);
  test_via_obs_t vobs;
  memset(&vobs, 0, sizeof(vobs));
  via_context_t via = {
    .cfg       = &cfg,
    .hall      = &hall,
    .on_event  = via_evt_cb,
    .event_ctx = &vobs,
  };
  uint8_t req[32];
  uint8_t rep[32];
  memset(req, 0, sizeof(req));
  req[0] = 0x07U; req[1] = 9U; req[2] = 1U; req[3] = 2U; /* BT slot 3 */
  via_process_packet(&via, req, rep);
  CHECK(cfg.rf_bt_slot == 2U && vobs.last_evt == VIA_EVT_RF_CONFIG_CHANGED);

  vobs.last_evt = VIA_EVT_NONE;
  req[2] = 2U; req[3] = 1U; /* 500Hz */
  via_process_packet(&via, req, rep);
  req[2] = 3U; req[3] = 1U; /* macOS */
  via_process_packet(&via, req, rep);
  req[2] = 4U; req[3] = 10U; /* 10 min sleep */
  via_process_packet(&via, req, rep);
  CHECK(cfg.rf_poll_rate_idx == 1U && cfg.mac_mode == 1U && cfg.sleep_timeout_min == 10U);

  req[2] = 5U; req[3] = 1U; /* RF pair trigger */
  via_process_packet(&via, req, rep);
  CHECK(vobs.last_evt == VIA_EVT_RF_PAIR);

  /* HUD status/options protocol alignment with stock TFT UART7 layout */
  tx_rec_t hud_rec;
  memset(&hud_rec, 0, sizeof(hud_rec));
  cfg.win_lock        = 1U;
  cfg.rgb_effect      = 2U;
  cfg.rgb_brightness  = 160U;
  cfg.side_brightness = 0x23U;
  hud_engine_t hud;
  hud_init_cfg_t hcfg = { .cfg = &cfg, .send_fn = record_tx, .send_ctx = &hud_rec };
  hud_engine_init(&hud, &hcfg);
  hud_status_t hst = {
    .active_layer = 1U,
    .battery_pct  = 85U,
    .host_leds    = 0x02U,
    .mac_mode     = true,
    .conn_mode    = TK75_CONN_BT,
    .bt_slot      = 2U,
    .rf_state     = SPI3_RF_STATE_CONNECTED,
    .charging     = true,
    .vbus_present = true,
  };
  hud_engine_set_status(&hud, &hst);
  hud_engine_tick(&hud, 100U);
  CHECK(hud_rec.count == 1U && hud_rec.cmds[0] == HUD_CMD_OPTIONS && hud_rec.lens[0] == 12U);
  CHECK(hud_rec.last_payload[0] == 2U && hud_rec.last_payload[1] == 1U && hud_rec.last_payload[2] == 2U &&
        hud_rec.last_payload[3] == 85U && hud_rec.last_payload[6] == 0x02U &&
        hud_rec.last_payload[10] == 1U && hud_rec.last_payload[11] == 1U);

  hud_engine_tick(&hud, 500U);
  CHECK(hud_rec.count == 2U && hud_rec.cmds[1] == HUD_CMD_STATUS && hud_rec.lens[1] == 18U);
  CHECK(hud_rec.last_payload[0] == 0U && hud_rec.last_payload[1] == 0U && hud_rec.last_payload[2] == 0U &&
        hud_rec.last_payload[8] == 2U && hud_rec.last_payload[9] == 4U &&
        hud_rec.last_payload[10] == 3U && hud_rec.last_payload[11] == 2U &&
        hud_rec.last_payload[12] == 1U && hud_rec.last_payload[13] == 5U &&
        hud_rec.last_payload[14] == 1U && hud_rec.last_payload[15] == 2U);

  hud_engine_tick(&hud, 600U);
  CHECK(hud_rec.count == 3U && hud_rec.cmds[2] == HUD_CMD_WAKE_READY);

  /* Typing must update WPM without emitting cmd=0x01 or putting WPM into TFT menu cursor p[0..2] */
  memset(&hud_rec, 0, sizeof(hud_rec));
  for (uint32_t t = 0U; t < 20U; t++) {
    hud_engine_note_keystroke(&hud, 700U + t * 20U);
  }
  hud_engine_tick(&hud, 1500U);
  CHECK(hud.wpm_current > 0U && hud_rec.count == 0U);

  /* Re-trigger status packet directly (USB full, not charging) */
  hst.conn_mode = TK75_CONN_USB;
  hst.charging  = false;
  hst.mac_mode  = false;
  hud_engine_set_status(&hud, &hst);
  hud_engine_tick(&hud, 1550U);
  CHECK(hud_rec.count == 1U && hud_rec.cmds[0] == HUD_CMD_OPTIONS);
  CHECK(hud_rec.last_payload[0] == 4U && hud_rec.last_payload[1] == 0U && hud_rec.last_payload[2] == 1U &&
        hud_rec.last_payload[10] == 0U);
  hud_engine_mark_options_dirty(&hud);
  hud_engine_tick(&hud, 1560U);
  hud_engine_tick(&hud, 1570U);
  CHECK(hud_rec.count == 3U && hud_rec.cmds[2] == HUD_CMD_STATUS);
  CHECK(hud_rec.last_payload[0] == 0U && hud_rec.last_payload[1] == 0U && hud_rec.last_payload[2] == 0U);

  return 0;
}

static int test_review_hardening(void)
{
  /* 1. SPI3 RF busy retry state machine preserves non-poll command across polls */
  spi3_rf_state_t rf;
  spi3_rf_state_init(&rf, SPI3_RF_MODE_BT1, 0U);
  uint8_t cmd_pkt[SPI3_RF_BUF_SIZE];
  uint8_t poll_pkt[SPI3_RF_BUF_SIZE];
  uint8_t keys6[6] = { KC_A, 0, 0, 0, 0, 0 };
  uint16_t cmd_len  = spi3_rf_build_boot_kbd(0x02U, keys6, cmd_pkt, (uint16_t)sizeof(cmd_pkt));
  uint16_t poll_len = spi3_rf_build_poll(poll_pkt, (uint16_t)sizeof(poll_pkt));

  spi3_rf_note_tx(&rf, cmd_pkt, cmd_len, SPI3_TX_NORMAL);
  CHECK(rf.retry_left == 3U && rf.last_cmd_len == cmd_len && rf.last_cmd_buf[0] == SPI3_RF_CMD_HID);

  /* Subsequent 0x09 poll must NOT clobber last_cmd_buf or retry_left */
  spi3_rf_note_tx(&rf, poll_pkt, poll_len, SPI3_TX_POLL);
  CHECK(rf.retry_left == 3U && rf.last_cmd_len == cmd_len && rf.last_cmd_buf[0] == SPI3_RF_CMD_HID);

  /* Busy responses decrement retry_left up to 3 times, then stop; OK clears retry_left */
  CHECK(spi3_rf_check_retry(&rf, SPI3_RF_RX_BUSY) && rf.retry_left == 2U);
  spi3_rf_note_tx(&rf, rf.last_cmd_buf, rf.last_cmd_len, SPI3_TX_RETRY);
  CHECK(rf.retry_left == 2U && rf.last_cmd_buf[0] == SPI3_RF_CMD_HID);
  CHECK(!spi3_rf_check_retry(&rf, SPI3_RF_RX_OK) && rf.retry_left == 0U);
  CHECK(!spi3_rf_check_retry(&rf, SPI3_RF_RX_BUSY));

  /* 2. Physical 3-way slider 15 ms consecutive-sample debouncer */
  mode_switch_debouncer_t db;
  memset(&db, 0, sizeof(db));
  mode_switch_state_t usb_st = mode_switches_decode(true, true, false, false);
  mode_switch_state_t bt_st  = mode_switches_decode(false, true, true, false);

  /* First sample seeds immediately */
  mode_switch_state_t cur = mode_switches_debounce_step(&db, usb_st);
  CHECK(cur.conn_mode == TK75_CONN_USB && cur.vbus_present);

  /* 14 ticks of BT followed by 1 glitch back to USB must NOT switch */
  for (uint8_t i = 0U; i < (MODE_SWITCH_DEBOUNCE_TICKS - 1U); i++) {
    cur = mode_switches_debounce_step(&db, bt_st);
    CHECK(cur.conn_mode == TK75_CONN_USB);
  }
  cur = mode_switches_debounce_step(&db, usb_st);
  CHECK(cur.conn_mode == TK75_CONN_USB && db.count == 0U);

  /* 15 consecutive ticks of BT commit the transition */
  for (uint8_t i = 0U; i < MODE_SWITCH_DEBOUNCE_TICKS; i++) {
    cur = mode_switches_debounce_step(&db, bt_st);
  }
  CHECK(cur.conn_mode == TK75_CONN_BT && !cur.vbus_present);

  /* 3. Win-Lock blocks modified GUI keycodes (LGUI(KC_R) = 0x0815) and TG(2) syncs mac_mode */
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);
  CHECK(cfg.keymap[1][5][9] == KC_MAC_TOG);
  cfg.win_lock = 1U;
  cfg.keymap[0][0][0] = (uint16_t)(0x0800U | KC_R); /* LGUI(KC_R) */
  cfg.keymap[0][0][1] = TG(2);

  uint16_t rest[HALL_SLOT_COUNT];
  uint16_t span[HALL_SLOT_COUNT];
  uint16_t adc[HALL_SLOT_COUNT];
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    rest[i] = 2400U;
    span[i] = 1000U;
    adc[i]  = 2400U;
  }
  hall_engine_t hall;
  hall_engine_init(&hall, rest, span);
  keymap_engine_t km;
  keymap_engine_init(&km, &cfg, &hall);

  adc[0] = 1500U; /* press (0,0) = LGUI(KC_R) */
  adc[1] = 1500U; /* press (0,1) = TG(2) */
  hall_engine_process_frame(&hall, adc);
  keymap_report_t rep;
  keymap_build_report(&km, &rep);
  CHECK((rep.modifiers & 0x88U) == 0U && rep.boot_keys[0] == KC_R);
  CHECK(km.mac_mode && cfg.mac_mode == 1U);

  /* 4. Macro player preserves explicit SS_DOWN(KC_LSFT) across SS_TAP(KC_A) */
  memset(cfg.macros, 0, sizeof(cfg.macros));
  uint8_t const seq[] = {
    0x01U, 0x02U, (uint8_t)KC_LSFT, /* SS_DOWN(KC_LSFT) */
    0x01U, 0x01U, (uint8_t)KC_A,    /* SS_TAP(KC_A) */
    0x01U, 0x01U, (uint8_t)KC_B,    /* SS_TAP(KC_B) */
    0x01U, 0x03U, (uint8_t)KC_LSFT, /* SS_UP(KC_LSFT) */
    0x00U
  };
  memcpy(cfg.macros, seq, sizeof(seq));
  macro_player_t mp;
  macro_player_init(&mp);
  macro_player_trigger(&mp, cfg.macros, 0U, 0U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 0U, &rep);  /* SS_DOWN(KC_LSFT) */
  CHECK(rep.modifiers == 0x02U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 5U, &rep);  /* SS_TAP(KC_A) down */
  CHECK(rep.modifiers == 0x02U && rep.boot_keys[0] == KC_A);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 16U, &rep); /* SS_TAP(KC_A) up -> LSFT must stay held! */
  CHECK(rep.modifiers == 0x02U && rep.pressed_count == 0U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 25U, &rep); /* SS_TAP(KC_B) down still shifted */
  CHECK(rep.modifiers == 0x02U && rep.boot_keys[0] == KC_B);

  /* 5. UART7 RX heartbeat preserves RGB cfg while out-of-range tft_page clamps */
  tx_rec_t hud_rec;
  memset(&hud_rec, 0, sizeof(hud_rec));
  hud_engine_t hud;
  hud_init_cfg_t hcfg = { .cfg = &cfg, .send_fn = record_tx, .send_ctx = &hud_rec };
  hud_engine_init(&hud, &hcfg);
  cfg.rgb_effect     = 1U;
  cfg.rgb_brightness = 160U;
  uart7_rx_frame_t bad_rx;
  memset(&bad_rx, 0, sizeof(bad_rx));
  bad_rx.cmd         = 0x81U;
  bad_rx.payload_len = 12U;
  CHECK(!hud_engine_handle_rx(&hud, &bad_rx));
  CHECK(cfg.rgb_effect == 1U && cfg.rgb_brightness == 160U);
  bad_rx.cmd = 0x82U;
  bad_rx.payload_len = 2U;
  bad_rx.payload[0]  = 1U;
  bad_rx.payload[1]  = 42U; /* out-of-range page */
  CHECK(hud_engine_handle_rx(&hud, &bad_rx) && cfg.tft_page == 0U);
  CHECK(hud.rx_count == 2U && hud.last_rx_cmd == 0x82U && hud.last_rx_payload[1] == 42U);

  /* 9. VIA Ext val_id=6 diagnostic snapshot getter */
  via_diag_t diag = {
    .conn_mode       = TK75_CONN_USB,
    .bt_slot         = 1U,
    .rf_state        = SPI3_RF_STATE_CONNECTED,
    .battery_pct     = 87U,
    .vbus_present    = 1U,
    .charging        = 1U,
    .rf_link_up      = 1U,
    .tft_rx_count    = 0x0102U,
    .tft_last_cmd    = 0x81U,
    .tft_last_len    = 4U,
    .tft_last_payload = { 0x11U, 0x22U, 0x33U, 0x44U },
    .hall_min_adc    = 1200U,
    .hall_max_adc    = 2500U,
    .pressed_count   = 2U,
    .cal_valid_count = 82U,
    .vbat_adc        = 1310U,
  };
  via_context_t via;
  memset(&via, 0, sizeof(via));
  via.cfg  = &cfg;
  via.diag = &diag;
  uint8_t dreq[VIA_RAW_EPSIZE], drep[VIA_RAW_EPSIZE];
  memset(dreq, 0, sizeof(dreq));
  dreq[0] = 0x08U; /* custom get */
  dreq[1] = 8U;    /* VIA_CH_EXT */
  dreq[2] = 6U;    /* val_id = diag */
  dreq[3] = 0U;    /* sub 0 */
  via_process_packet(&via, dreq, drep);
  CHECK(drep[0] == 0x08U && drep[7] == 87U && drep[8] == 1U && drep[17] == 82U);
  CHECK(((uint16_t)drep[11] << 8U | drep[12]) == 1200U);
  CHECK(((uint16_t)drep[19] << 8U | drep[20]) == 1310U);

  dreq[3] = 1U; /* sub 1 */
  via_process_packet(&via, dreq, drep);
  CHECK(((uint16_t)drep[4] << 8U | drep[5]) == 0x0102U && drep[6] == 0x81U && drep[8] == 0x11U);

  return 0;
}

static int test_battery_curve_and_gauge(void)
{
  /* 1. Verify stock tk75_battery_adc_to_pct piecewise curve */
  CHECK(tk75_battery_adc_to_pct(0U) == 1U);
  CHECK(tk75_battery_adc_to_pct(1109U) == 1U);
  CHECK(tk75_battery_adc_to_pct(1110U) == 1U);
  CHECK(tk75_battery_adc_to_pct(1195U) == 20U);
  CHECK(tk75_battery_adc_to_pct(1196U) == 20U);
  CHECK(tk75_battery_adc_to_pct(1310U) == 58U);
  CHECK(tk75_battery_adc_to_pct(1432U) == 99U);
  CHECK(tk75_battery_adc_to_pct(1433U) == 100U);
  CHECK(tk75_battery_adc_to_pct(2600U) == 100U);

  /* 2. USB before valid ADC sample stays 100% uninitialized, then seeds on first valid reading */
  tk75_battery_gauge_t g;
  tk75_battery_gauge_init(&g);
  CHECK(tk75_battery_gauge_step(&g, 0U, true, false) == 100U);
  CHECK(!g.initialized);
  CHECK(tk75_battery_gauge_step(&g, 1310U, true, true) == 58U);
  CHECK(g.initialized);

  /* 3. Monotonic UP on VBUS: dip while charging does not drop pct */
  for (uint8_t i = 0U; i < TK75_BATT_HYST_CONFIRM_TICKS + 2U; i++) {
    CHECK(tk75_battery_gauge_step(&g, 1200U, true, true) == 58U);
  }
  /* Sustained higher ADC climbs by +1% after confirm ticks */
  for (uint8_t i = 0U; i < TK75_BATT_HYST_CONFIRM_TICKS - 1U; i++) {
    CHECK(tk75_battery_gauge_step(&g, 1340U, true, true) == 58U);
  }
  CHECK(tk75_battery_gauge_step(&g, 1340U, true, true) == 59U);

  /* 4a. Charging 99% cap & promotion to 100% when charger IC terminates (!charging && inst >= 92) */
  tk75_battery_gauge_init(&g);
  CHECK(tk75_battery_gauge_step(&g, 1432U, true, true) == 99U);
  CHECK(tk75_battery_gauge_is_charging(&g));
  for (uint8_t i = 0U; i < TK75_BATT_HYST_CONFIRM_TICKS + 2U; i++) {
    CHECK(tk75_battery_gauge_step(&g, 1432U, true, true) == 99U);
    CHECK(tk75_battery_gauge_is_charging(&g));
  }
  for (uint8_t i = 0U; i < TK75_BATT_HYST_CONFIRM_TICKS - 1U; i++) {
    CHECK(tk75_battery_gauge_step(&g, 1422U, true, false) == 99U);
  }
  CHECK(tk75_battery_gauge_step(&g, 1422U, true, false) == 100U);
  CHECK(!tk75_battery_gauge_is_charging(&g));

  /* 4b. Load-compensated VBAT + CV-full 100% promotion when system load keeps PC14 HIGH */
  tk75_vbat_load_t load_off    = { false, 0U,  false, 0U };
  tk75_vbat_load_t load_tft    = { true,  10U, false, 0U };
  tk75_vbat_load_t load_rgb20  = { true,  10U, true,  12546U }; /* 82 keys solid white @ 51 */
  tk75_vbat_load_t load_rgb100 = { true,  10U, true,  62730U }; /* 82 keys solid white @ 255 */
  CHECK(tk75_battery_compensate_vbat(1450U, &load_off) == 1450U);
  CHECK(tk75_battery_compensate_vbat(1440U, &load_tft) >= 1448U);
  CHECK(tk75_battery_compensate_vbat(1417U, &load_rgb20) >= 1440U);
  CHECK(tk75_battery_compensate_vbat(1338U, &load_rgb100) >= 1438U);

  tk75_battery_gauge_init(&g);
  uint16_t v_full = tk75_battery_compensate_vbat(1417U, &load_rgb20);
  CHECK(tk75_battery_gauge_step(&g, v_full, true, true) == 99U); /* tick 1/8 */
  CHECK(tk75_battery_gauge_is_charging(&g));
  for (uint8_t i = 0U; i < TK75_BATT_FULL_CONFIRM_TICKS - 2U; i++) {
    CHECK(tk75_battery_gauge_step(&g, v_full, true, true) == 99U);
    CHECK(tk75_battery_gauge_is_charging(&g));
  }
  CHECK(tk75_battery_gauge_step(&g, v_full, true, true) == 100U); /* tick 8/8 -> CV full! */
  CHECK(!tk75_battery_gauge_is_charging(&g));                     /* switches bolt -> USB plug */
  /* Unplugging USB clears charge_complete & holds 100% cleanly in battery mode */
  CHECK(tk75_battery_gauge_step(&g, 1410U, false, false) == 100U);
  CHECK(!tk75_battery_gauge_is_charging(&g) && !g.charge_complete);

  /* 5. Unplug grace period + transient glitch immunity + sustained discharge decrement */
  tk75_battery_gauge_init(&g);
  CHECK(tk75_battery_gauge_step(&g, 1365U, true, false) == 77U);
  CHECK(tk75_battery_gauge_step(&g, 1250U, false, false) == 77U); /* Edge -> unplug grace */
  for (uint8_t i = 0U; i < TK75_BATT_UNPLUG_GRACE_TICKS; i++) {
    CHECK(tk75_battery_gauge_step(&g, 1250U, false, false) == 77U);
  }
  /* Glitch sample < 600 must be ignored without corrupting percentage */
  CHECK(tk75_battery_gauge_step(&g, 150U, false, false) == 77U);
  for (uint8_t i = 0U; i < TK75_BATT_HYST_CONFIRM_TICKS - 1U; i++) {
    CHECK(tk75_battery_gauge_step(&g, 1250U, false, false) == 77U);
  }
  CHECK(tk75_battery_gauge_step(&g, 1250U, false, false) == 76U);
  return 0;
}

static int test_tft_menu_navigation(void)
{
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);
  tx_rec_t rec;
  memset(&rec, 0, sizeof(rec));
  hud_engine_t hud;
  hud_init_cfg_t hcfg = { .cfg = &cfg, .send_fn = record_tx, .send_ctx = &rec };
  hud_engine_init(&hud, &hcfg);

  /* Start on Home (0) -> OK enters top-level menu (1) -> OK enters Pic sub (0x11) -> OK returns to 1 */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 50U));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU && hud.menu_dirty);
  hud_engine_tick(&hud, 50U);
  CHECK(rec.count == 1U && rec.cmds[0] == HUD_CMD_STATUS && rec.last_payload[0] == HUD_MENU_PIC_MENU);

  /* Stock cmd=0x81 ACK echo carries cursor at payload[1..2], slot at [3], must NOT reset to 0 */
  uart7_rx_frame_t ack81 = { .cmd = 0x81U, .payload_len = 12U, .payload = { 0 } };
  ack81.payload[1] = HUD_MENU_PIC_MENU;
  ack81.payload[2] = 0U;
  CHECK(!hud_engine_handle_rx(&hud, &ack81));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU);

  /* Pushing OK on PicMenu (1) with no pictures enters PicSub (0x11), then OK/turn returns to 1 */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 60U));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_SUB);
  /* Stale 0x81 from before tick must not overwrite unsent menu_dirty cursor */
  CHECK(!hud_engine_handle_rx(&hud, &ack81));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_SUB);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 70U));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU);
  hud_engine_tick(&hud, 100U);

  /* Navigate Home(1) -> Side(3) -> RGB(4) -> OK -> RGB FX(0x14) -> Bright(0x34) -> Edit(0x134) */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 110U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 120U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 130U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_FX);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 140U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_COLOR);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 150U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_BRIGHT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 160U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_ED_BRT);

  /* Step brightness 3 -> 4 (255), then OK back -> RGB Back(0x54) -> Root(4) */
  CHECK(hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 170U));
  CHECK(cfg.rgb_brightness == 255U);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 180U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_BRIGHT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 190U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_SPEED);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 200U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_BACK);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 210U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_ROOT);

  /* Navigate Root RGB(4) -> OS(5) -> OK(0x15) -> NEXT toggles Mac mode */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 220U));
  CHECK(hud.menu_cursor == HUD_MENU_OS_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 230U));
  CHECK(hud.menu_cursor == HUD_MENU_OS);
  CHECK(hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 240U));
  CHECK(cfg.mac_mode == 1U && hud.status.mac_mode && hud.options_dirty && hud.menu_dirty);

  /* menu_dirty flushes cmd=0x01 BEFORE cmd=0x02 options_dirty */
  memset(&rec, 0, sizeof(rec));
  hud_engine_tick(&hud, 240U);
  CHECK(rec.count == 1U && rec.cmds[0] == HUD_CMD_STATUS && rec.last_payload[7] == 1U && rec.last_payload[10] == 4U);
  hud_engine_tick(&hud, 241U);
  CHECK(rec.count == 2U && rec.cmds[1] == HUD_CMD_OPTIONS && rec.last_payload[4] == 1U && rec.last_payload[10] == 1U);

  /* Side submenu edit matches stock 0x0133=Bright(p[14], 0..4) and 0x0143=Speed(p[15], 0..3) */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 250U));
  CHECK(hud.menu_cursor == HUD_MENU_OS_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 260U));
  CHECK(hud.menu_cursor == HUD_MENU_RGB_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 270U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 280U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_FX);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 290U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_BACK);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 300U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_SPEED);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 305U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_BRIGHT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 310U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_ED_BRT);
  CHECK(hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 320U));
  CHECK(cfg.side_brightness == 255U);

  /* Side Speed edit (0x0143) -> step 0..3, status packet p[14]=side_bright(4), p[15]=side_speed(3) */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 330U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_BRIGHT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 340U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_SPEED);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 350U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_ED_SPD);
  CHECK(hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 360U));
  CHECK(cfg.side_speed == 255U);
  memset(&rec, 0, sizeof(rec));
  hud_engine_tick(&hud, 360U);
  CHECK(rec.count == 1U && rec.cmds[0] == HUD_CMD_STATUS && rec.last_payload[14] == 4U && rec.last_payload[15] == 3U);

  /* Exiting top menu ends returns to Home cleanly instead of trapping user in menu loop */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 370U));
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 375U));
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 380U));
  CHECK(hud.menu_cursor == HUD_MENU_SIDE_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 385U));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU);
  CHECK(hud_engine_is_menu_open(&hud));
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 390U));
  CHECK(hud.menu_cursor == HUD_MENU_HOME && !hud_engine_is_menu_open(&hud));

  /* Opening menu then explicit hud_engine_close_menu returns cleanly to Home */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 392U));
  CHECK(hud.menu_cursor == HUD_MENU_INFO_ROOT && hud_engine_is_menu_open(&hud));
  hud_engine_close_menu(&hud);
  CHECK(hud.menu_cursor == HUD_MENU_HOME && !hud_engine_is_menu_open(&hud));

  /* RX 0x85 PicView sync (payload[1..2]=cursor LE, payload[3]=slot, payload[4..9]=pic_exists) & timeout */
  uart7_rx_frame_t rx = { .cmd = 0x85U, .payload_len = 12U, .payload = { 0 } };
  rx.payload[1] = HUD_MENU_PIC_VIEW;
  rx.payload[3] = 2U;
  rx.payload[4 + 2] = 1U;
  rx.payload[4 + 4] = 1U;
  CHECK(!hud_engine_handle_rx(&hud, &rx));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_VIEW && hud.menu_pic_slot == 2U && hud.menu_trigger == 0U);

  /* From PicView(2): NEXT moves to next populated slot 4, then OK opens menu and times out back to PicView(2, slot=4) */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 400U));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_VIEW && hud.menu_pic_slot == 4U);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 500U));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU);
  hud_engine_tick(&hud, 500U + HUD_MENU_TIMEOUT_MS);
  CHECK(hud.menu_cursor == HUD_MENU_PIC_VIEW && hud.menu_pic_slot == 4U);

  /* Cold boot unsolicited 0x81/0x85 with stale 0x0015 (HUD_MENU_OS) must NEVER open menu */
  hud_engine_init(&hud, &hcfg);
  for (uint32_t t = 0U; t < 3U; t++) {
    hud_engine_tick(&hud, t);
  }
  memset(&rec, 0, sizeof(rec));
  uart7_rx_frame_t boot_rx = { .cmd = 0x81U, .payload_len = 12U, .payload = { 0 } };
  boot_rx.payload[1] = HUD_MENU_OS;
  CHECK(!hud_engine_handle_rx(&hud, &boot_rx));
  CHECK(hud.menu_cursor == HUD_MENU_HOME && !hud_engine_is_menu_open(&hud) && hud.menu_dirty);
  boot_rx.cmd = 0x85U;
  CHECK(!hud_engine_handle_rx(&hud, &boot_rx));
  CHECK(hud.menu_cursor == HUD_MENU_HOME && !hud_engine_is_menu_open(&hud) && hud.menu_dirty);
  hud_engine_tick(&hud, 20U);
  CHECK(rec.count == 1U && rec.cmds[0] == HUD_CMD_STATUS && rec.last_payload[0] == HUD_MENU_HOME);

  /* After opening menu + TX, stale 0x81(HOME) is ignored during sync holdoff; matching RX clears holdoff */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 50U));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU);
  hud_engine_tick(&hud, 55U);
  CHECK(!hud.menu_dirty && hud.menu_sync_ticks > 0U);
  uart7_rx_frame_t stale_home = { .cmd = 0x81U, .payload_len = 12U, .payload = { 0 } };
  CHECK(!hud_engine_handle_rx(&hud, &stale_home));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU && hud_engine_is_menu_open(&hud));
  uart7_rx_frame_t ack_menu = { .cmd = 0x81U, .payload_len = 12U, .payload = { 0 } };
  ack_menu.payload[1] = HUD_MENU_PIC_MENU;
  CHECK(!hud_engine_handle_rx(&hud, &ack_menu));
  CHECK(hud.menu_cursor == HUD_MENU_PIC_MENU && hud.menu_sync_ticks == 0U);

  /* Explicit close during sync holdoff ignores stale 0x85(0x0015) */
  hud_engine_close_menu(&hud);
  hud_engine_tick(&hud, 60U);
  CHECK(!hud_engine_is_menu_open(&hud) && hud.menu_sync_ticks > 0U);
  CHECK(!hud_engine_handle_rx(&hud, &boot_rx));
  CHECK(hud.menu_cursor == HUD_MENU_HOME && !hud_engine_is_menu_open(&hud));

  /* Cursor 0x0017 (HUD_MENU_INFO): TFT screen brightness toggles p[4] on PREV (CCW) and p[5] on NEXT (CW) */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 70U));
  CHECK(hud.menu_cursor == HUD_MENU_INFO_ROOT);
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 75U));
  CHECK(hud.menu_cursor == HUD_MENU_INFO);
  hud_engine_tick(&hud, 75U);
  CHECK(rec.last_payload[0] == HUD_MENU_INFO && rec.last_payload[4] == 0U && rec.last_payload[5] == 0U);

  /* Double-step before tick must not cancel out the pending toggle edge */
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 80U));
  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 81U));
  hud_engine_tick(&hud, 82U);
  CHECK(rec.last_payload[0] == HUD_MENU_INFO && rec.last_payload[4] == 1U && rec.last_payload[5] == 0U);

  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_PREV, 85U));
  hud_engine_tick(&hud, 86U);
  CHECK(rec.last_payload[4] == 0U && rec.last_payload[5] == 0U);

  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 90U));
  hud_engine_tick(&hud, 91U);
  CHECK(rec.last_payload[4] == 0U && rec.last_payload[5] == 1U);

  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_NEXT, 95U));
  hud_engine_tick(&hud, 96U);
  CHECK(rec.last_payload[4] == 0U && rec.last_payload[5] == 0U);

  CHECK(!hud_engine_menu_input(&hud, HUD_MENU_ACT_OK, 100U));
  CHECK(hud.menu_cursor == HUD_MENU_INFO_ROOT);
  return 0;
}

static int test_pixel_hud(void)
{
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);

  tx_rec_t rec;
  memset(&rec, 0, sizeof(rec));
  pixel_hud_t px;
  pixel_hud_init(&px, &cfg, record_tx, &rec);

  /* Probe cmd=0x25 on startup before detected */
  pixel_hud_tick(&px, 600U);
  CHECK(!pixel_hud_is_active(&px));
  CHECK(rec.count == 1U && rec.cmds[0] == PIXEL_HUD_CMD_VERSION);

  /* Simulated U24 reply 0xA5 v1.1 activates Pixel HUD and enables manual mode */
  uart7_rx_frame_t rx = { .cmd = 0xA5U, .payload_len = 3U, .payload = { 0x00U, 1U, 1U } };
  pixel_hud_on_rx(&px, &rx);
  CHECK(pixel_hud_is_active(&px) && px.patch_major == 1U && px.patch_minor == 1U);

  /* Drain initial manual-mode + HOME page render + post-flush brightness (with ACK pacing) */
  uart7_rx_frame_t ack = { .cmd = 0xA6U, .payload_len = 1U, .payload = { 0x00U } };
  for (uint32_t t = 610U; t < 800U; t += 10U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(px.applied_tft_brt == cfg.tft_brightness);

  /* Post-boot brightness re-assert fires cleanly at +300/900/2000ms (beats U24 stock backlight ramp) */
  pixel_hud_t px_brt;
  pixel_hud_init(&px_brt, &cfg, record_tx, &rec);
  pixel_hud_on_rx(&px_brt, &rx);
  uint32_t brt_tx_cnt = 0U;
  for (uint32_t t = 100U; t < 2600U; t += 10U) {
    rec.count = 0U;
    pixel_hud_tick(&px_brt, t);
    pixel_hud_on_rx(&px_brt, &ack);
    if (rec.count > 0U && rec.cmds[0] == PIXEL_HUD_CMD_BRIGHTNESS &&
        rec.last_payload[0] == cfg.tft_brightness && rec.last_payload[1] == 1U) {
      brt_tx_cnt++;
    }
  }
  CHECK(px_brt.applied_tft_brt == cfg.tft_brightness && brt_tx_cnt >= 3U && px_brt.brt_reapply_left == 0U);

  /* Live Hall press drives plunger animation via atomic 0x26 batch rect */
  pixel_hud_live_t live = {
    .active_layer  = 0U,
    .battery_pct   = 95U,
    .conn_mode     = 0U,
    .wpm           = 72U,
    .max_travel_um = 3000U,
    .any_actuated  = true,
  };
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 850U; t < 1100U; t += 30U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(px.anim_plunger_px > 0U && px.prev_actuated);

  /* Power-state transitions (Battery -> USB charging bolt -> USB full plug -> Battery) update header cleanly */
  uint32_t h_bat = px.prev_header_hash;
  live.vbus_present = true;
  live.charging     = true;
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1120U; t < 1160U; t += 10U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  uint32_t h_chg = px.prev_header_hash;
  CHECK(h_chg != h_bat);
  live.charging = false; /* USB full (plug icon) */
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1190U; t < 1230U; t += 10U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  uint32_t h_usb = px.prev_header_hash;
  CHECK(h_usb != h_chg && h_usb != h_bat);
  live.vbus_present = false; /* Battery mode */
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1260U; t < 1300U; t += 10U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(px.prev_header_hash == h_bat);

  /* Fullscreen pixel-art macro recording takeover animates reels & restores HUD cleanly on exit */
  live.macro_recording = true;
  live.macro_rec_slot  = 0U;
  live.macro_rec_bytes = 9U;
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1310U; t < 1380U; t += 5U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(px.prev_macro_rec && px.prev_rec_bytes == 9U);
  live.macro_rec_bytes = 18U;
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1382U; t < 1392U; t += 2U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(px.rec_anim_phase >= 1U && px.prev_rec_bytes == 18U);
  live.macro_recording = false;
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1394U; t < 1399U || px.q_count > 0U; t++) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(!px.prev_macro_rec);

  /* Idle pixel-art screensaver activates after 30s idle (even with 450um warm Hall drift) and wakes on typing */
  live.any_actuated  = false;
  live.max_travel_um = 450U;
  live.idle_ms       = 30000U;
  live.rtc_valid     = true;
  live.rtc_hour      = 20U;
  live.rtc_min       = 15U;
  live.rtc_sec       = 30U;
  live.peak_wpm      = 92U;
  live.session_keys  = 420U;
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1196U; t < 1198U || px.q_count > 0U; t++) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(px.screensaver_active && px.prev_saver_min == 15U);

  /* Typing resets idle -> wakes immediately back to HOME (WPM) */
  live.idle_ms       = 0U;
  live.any_actuated  = true;
  live.max_travel_um = 2000U;
  pixel_hud_set_live(&px, &live);
  for (uint32_t t = 1199U; px.screensaver_active || px.q_count > 0U; t++) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(!px.screensaver_active);

  /* VIA ch=7 val_id=9 toggles tft_screensaver cleanly */
  via_context_t via_sv;
  memset(&via_sv, 0, sizeof(via_sv));
  via_sv.cfg = &cfg;
  uint8_t sv_req[VIA_RAW_EPSIZE] = { 0x07U, 7U, 9U, 0U };
  uint8_t sv_rep[VIA_RAW_EPSIZE] = { 0 };
  via_process_packet(&via_sv, sv_req, sv_rep);
  CHECK(cfg.tft_screensaver == 0U);
  sv_req[0] = 0x08U;
  via_process_packet(&via_sv, sv_req, sv_rep);
  CHECK(sv_rep[3] == 0U);
  sv_req[0] = 0x07U; sv_req[3] = 1U;
  via_process_packet(&via_sv, sv_req, sv_rep);
  CHECK(cfg.tft_screensaver == 1U);

  /* Open menu -> Brightness page row 1 (TFT), edit down by 10% */
  pixel_hud_open_menu(&px, 1200U);
  CHECK(pixel_hud_is_menu_open(&px) && px.page == PIXEL_PAGE_BRIGHTNESS && px.focus_row == 1U);
  uint8_t brt_before = cfg.tft_brightness;
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1210U));
  CHECK(px.editing);
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_PREV, 1220U));
  CHECK(cfg.tft_brightness == (uint8_t)(brt_before - 10U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1230U));
  CHECK(!px.editing);

  /* Move to row 2 (RGB) and adjust brightness */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1240U));
  CHECK(px.focus_row == 2U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1250U));
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1260U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1270U));

  /* Move to row 3 (SID) and adjust sidelight brightness */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1280U));
  CHECK(px.focus_row == 3U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1290U));
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_PREV, 1300U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1310U));

  /* Close menu -> returns cleanly to HOME with left wipe animation (no uint8 underflow on x) */
  pixel_hud_close_menu(&px);
  CHECK(!pixel_hud_is_menu_open(&px) && px.page == PIXEL_PAGE_HOME && px.wipe_step == 1U);
  uint32_t saw_left_zero = 0U;
  for (uint32_t t = 1320U; t < 1600U; t += 10U) {
    rec.count = 0U;
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
    if (rec.count > 0U && rec.cmds[0] == PIXEL_HUD_CMD_BATCH_RECT) {
      uint8_t n = rec.last_payload[1];
      for (uint8_t i = 0U; i < n; i++) {
        uint8_t rx_x = rec.last_payload[2U + i * 6U];
        CHECK(rx_x < 128U);
        if (rx_x == 0U && rec.last_payload[2U + i * 6U + 1U] == 19U) {
          saw_left_zero = 1U;
        }
      }
    }
  }
  CHECK(saw_left_zero == 1U && px.wipe_step == 0U);

  /* Switch UI: PIXEL -> STOCK via SYSTEM row 3 -> clears full 128x128 to 0x18E4 then sends MANUAL=0 */
  pixel_hud_open_menu(&px, 1600U);
  for (uint32_t t = 1610U; t < 1800U; t += 10U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  /* From BRIGHTNESS row 1 -> row 0 (tab bar) -> verify PREV (left) wraps cleanly across all 5 pages */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_PREV, 1810U));
  CHECK(px.focus_row == 0U && px.page == PIXEL_PAGE_BRIGHTNESS);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_PREV, 1812U));
  CHECK(px.page == PIXEL_PAGE_HOME);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_PREV, 1814U));
  CHECK(px.page == PIXEL_PAGE_SYSTEM);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_PREV, 1816U));
  CHECK(px.page == PIXEL_PAGE_MACRO);

  /* On MACRO page: edit SPD, then select KEYS row -> pick slot 1 (V) -> OK triggers playback */
  uint8_t const v_macro[3] = { 0x01U, 0x01U, (uint8_t)KC_Z };
  CHECK(macro_recorder_save_slot(cfg.macros, 1U, v_macro, 3U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1832U));
  CHECK(px.focus_row == 1U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1834U));
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1836U));
  CHECK(cfg.macro_speed_idx == (uint8_t)MACRO_SPEED_MED);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1837U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1838U)); /* row 2 */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1839U)); /* row 3 (KEYS) */
  CHECK(px.focus_row == 3U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1840U));   /* enter KEYS edit */
  CHECK(px.editing && px.macro_sel_slot == 0U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1841U)); /* select slot 1 (V) */
  CHECK(px.macro_sel_slot == 1U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1842U));   /* trigger playback of slot 1 */
  CHECK(!px.editing && px.macro_play_req == 1U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1843U)); /* wrap row 3 -> row 0 (tab bar) */
  CHECK(px.focus_row == 0U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1844U));
  CHECK(px.page == PIXEL_PAGE_SYSTEM);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1845U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1850U)); /* row 2: OS */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1855U)); /* row 3: SAVR */
  CHECK(px.focus_row == 3U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1858U));
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1860U));  /* toggle SAVR OFF */
  CHECK(cfg.tft_screensaver == 0U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1862U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1865U)); /* row 4: UI */
  CHECK(px.focus_row == 4U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 1870U));
  for (uint32_t t = 1871U; px.q_count > 0U; t++) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  rec.count = 0U;
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 1880U));
  CHECK(!pixel_hud_is_active(&px) && px.stock_ui_mode && !px.menu_open && px.wipe_step == 0U);
  for (uint32_t t = 1890U; t < 1940U; t += 10U) {
    pixel_hud_tick(&px, t);
    pixel_hud_on_rx(&px, &ack);
  }
  CHECK(rec.count >= 2U);
  CHECK(rec.cmds[rec.count - 2U] == PIXEL_HUD_CMD_FILL_RECT);
  CHECK(rec.cmds[rec.count - 1U] == PIXEL_HUD_CMD_MANUAL && rec.last_payload[0] == 0U);
  return 0;
}

static int test_xtrfy_macro_recorder(void)
{
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);

  /* Default Layer 1 C,V,B,N,M map to VIA-visible MACRO(0)..MACRO(4) */
  for (uint8_t i = 0U; i < 5U; i++) {
    CHECK(cfg.keymap[1][4][4U + i] == MACRO(i));
  }

  /* Seed slot M1 so we can verify recording M0 preserves M1 untouched */
  uint8_t const m1_seed[3] = { 0x01U, 0x01U, (uint8_t)KC_Z };
  CHECK(macro_recorder_save_slot(cfg.macros, 1U, m1_seed, 3U));
  CHECK(!macro_recorder_slot_has_data(cfg.macros, 0U));
  CHECK(macro_recorder_slot_has_data(cfg.macros, 1U));

  uint16_t rest[HALL_SLOT_COUNT], span[HALL_SLOT_COUNT], adc[HALL_SLOT_COUNT];
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    rest[i] = 2400U; span[i] = 1000U; adc[i] = 2400U;
  }
  hall_engine_t hall;
  hall_engine_init(&hall, rest, span);

  macro_recorder_t rec;
  macro_recorder_init(&rec);

  /* 1. Short tap Fn+C (< 1.8s) plays on release without starting recording */
  macro_key_event_t down = { .macro_id = 0U, .pressed = true, .fn_active = true, .now_ms = 100U };
  macro_key_event_t up   = { .macro_id = 0U, .pressed = false, .fn_active = true, .now_ms = 300U };
  CHECK(!macro_recorder_on_macro_key(&rec, &down));
  CHECK(macro_recorder_on_macro_key(&rec, &up));
  CHECK(!macro_recorder_is_capturing(&rec));

  /* 2. Hold Fn+C for 1.8s enters Xtrfy K5-style recording */
  down.now_ms = 1000U;
  CHECK(!macro_recorder_on_macro_key(&rec, &down));
  adc[5U * HALL_COLS + 10U] = 1400U; /* Fn held */
  adc[4U * HALL_COLS + 4U]  = 1400U; /* C held */
  hall_engine_process_frame(&hall, adc);
  macro_rec_tick_ctx_t ctx = { .hall = &hall, .cfg = &cfg, .layer_state = 3UL, .now_ms = 2800U };
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_STARTED);
  CHECK(macro_recorder_is_capturing(&rec) && rec.state == MACRO_REC_WAIT_START_RELEASE);

  /* Release Fn + C -> transitions into active MACRO_REC_RECORDING (pump EMA frames) */
  adc[5U * HALL_COLS + 10U] = 2400U;
  adc[4U * HALL_COLS + 4U]  = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 2820U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE && rec.state == MACRO_REC_RECORDING);

  /* Record chord LCtrl + A */
  adc[5U * HALL_COLS + 0U] = 1400U; /* LCtrl down */
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 2840U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE);

  adc[3U * HALL_COLS + 1U] = 1400U; /* A down */
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 2860U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE);

  adc[3U * HALL_COLS + 1U] = 2400U; /* A up -> collapses to SS_TAP(KC_A) */
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 2880U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE);

  adc[5U * HALL_COLS + 0U] = 2400U; /* LCtrl up */
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 2900U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE);

  /* Press Fn once -> stops & saves to VIA macro slot M0 */
  adc[5U * HALL_COLS + 10U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 2920U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_SAVED);
  CHECK(rec.state == MACRO_REC_WAIT_STOP_RELEASE);

  /* Release Fn -> returns to IDLE */
  adc[5U * HALL_COLS + 10U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 2940U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE && !macro_recorder_is_capturing(&rec));

  /* Verify native VIA v3 bytecode in M0 AND preserved M1 */
  CHECK(macro_recorder_slot_has_data(cfg.macros, 0U));
  CHECK(macro_recorder_slot_has_data(cfg.macros, 1U));
  /* Expected M0: DOWN(LCTL) TAP(A) UP(LCTL) \0 TAP(Z) \0 */
  uint8_t const expect_prefix[] = {
    0x01U, 0x02U, (uint8_t)KC_LCTL,
    0x01U, 0x01U, (uint8_t)KC_A,
    0x01U, 0x03U, (uint8_t)KC_LCTL,
    0x00U,
    0x01U, 0x01U, (uint8_t)KC_Z,
    0x00U,
  };
  CHECK(memcmp(cfg.macros, expect_prefix, sizeof(expect_prefix)) == 0);

  /* Playback via macro_player produces LCtrl + A */
  macro_player_t mp;
  macro_player_init(&mp);
  macro_player_trigger(&mp, cfg.macros, 0U, 0U);
  keymap_report_t rep;
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 0U, &rep);  /* DOWN(LCTL) */
  CHECK(rep.modifiers == 0x01U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 5U, &rep);  /* TAP(A) down */
  CHECK(rep.modifiers == 0x01U && rep.boot_keys[0] == KC_A);

  /* Clearing M0 (len=0) still keeps M1 intact */
  CHECK(macro_recorder_save_slot(cfg.macros, 0U, NULL, 0U));
  CHECK(!macro_recorder_slot_has_data(cfg.macros, 0U));
  CHECK(macro_recorder_slot_has_data(cfg.macros, 1U));

  /* Expanded 2KB macro pool stores long macros (>600 B / 200+ events) while preserving other slots */
  uint8_t big_macro[600];
  for (uint16_t i = 0U; i < 200U; i++) {
    big_macro[i * 3U + 0U] = 0x01U;
    big_macro[i * 3U + 1U] = 0x01U;
    big_macro[i * 3U + 2U] = (uint8_t)KC_A;
  }
  CHECK(macro_recorder_save_slot(cfg.macros, 0U, big_macro, (uint16_t)sizeof(big_macro)));
  CHECK(macro_recorder_slot_has_data(cfg.macros, 0U));
  CHECK(macro_recorder_slot_has_data(cfg.macros, 1U));
  CHECK(cfg.macros[599] == (uint8_t)KC_A && cfg.macros[600] == 0U && cfg.macros[603] == (uint8_t)KC_Z);

  /* Default Layer 1 ',' and '.' map to Macro Speed Down / Up */
  CHECK(cfg.keymap[1][4][9] == KC_MACRO_SPD_DN);
  CHECK(cfg.keymap[1][4][10] == KC_MACRO_SPD_UP);

  /* Speed presets scale both tap timing AND SS_DELAY multiplier */
  uint8_t const delay_macro[] = {
    0x01U, 0x01U, (uint8_t)KC_A,
    0x01U, 0x04U, 40U, 0U, /* SS_DELAY(40ms) */
    0x01U, 0x01U, (uint8_t)KC_B,
    0x00U,
  };
  CHECK(macro_recorder_save_slot(cfg.macros, 0U, delay_macro, (uint16_t)sizeof(delay_macro) - 1U));

  macro_player_set_speed(&mp, (uint8_t)MACRO_SPEED_FAST); /* 2ms tap, 2ms gap, 0.25x delay (10ms) */
  macro_player_trigger(&mp, cfg.macros, 0U, 100U);
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 100U, &rep); /* TAP(A) down, until 102 */
  CHECK(rep.boot_keys[0] == KC_A && mp.delay_until_ms == 102U);

  /* Live macro typing bitmap latches KC_A and lights exact 'A' LED on keyboard */
  uint8_t typed_bmp[MACRO_TYPED_BITMAP_BYTES];
  macro_player_consume_typed_bitmap(&mp, typed_bmp);
  CHECK((typed_bmp[KC_A >> 3U] & (uint8_t)(1U << (KC_A & 7U))) != 0U);

  rgb_effects_state_t fx;
  memset(&fx, 0, sizeof(fx));
  rgb_grb_t leds[RGB_LED_COUNT];
  cfg.rgb_enabled    = 1U;
  cfg.rgb_effect     = RGB_FX_SOLID;
  cfg.rgb_hue        = 0U;
  cfg.rgb_sat        = 255U;
  cfg.rgb_brightness = 200U;
  rgb_render_ctx_t rctx = {
    .fx                 = &fx,
    .cfg                = &cfg,
    .hall               = &hall,
    .active_layer       = 0U,
    .layer_mask         = 1UL,
    .macro_typed_bitmap = typed_bmp,
    .now_ms             = 100U,
  };
  rgb_effects_render(&rctx, leds);
  uint8_t   a_led   = rgb_matrix_to_led_idx(3U, 1U); /* 'A' key */
  uint8_t   b_led   = rgb_matrix_to_led_idx(4U, 6U); /* 'B' key (not yet typed) */
  rgb_grb_t hl_col  = rgb_hsv_to_grb(128U, 255U, 200U);
  rgb_grb_t base_c  = rgb_hsv_to_grb(0U, 255U, 200U);
  CHECK(leds[a_led].r == hl_col.r && leds[a_led].g == hl_col.g && leds[a_led].b == hl_col.b);
  CHECK(leds[b_led].r == base_c.r && leds[b_led].g == base_c.g && leds[b_led].b == base_c.b);

  /* Finish TAP(A) release (102 -> gap until 104) then SS_DELAY(40ms * 0.25 = 10ms -> until 114) */
  memset(&rep, 0, sizeof(rep));
  macro_player_step(&mp, cfg.macros, 102U, &rep);
  CHECK(mp.delay_until_ms == 104U);
  macro_player_step(&mp, cfg.macros, 104U, &rep);
  CHECK(mp.delay_until_ms == 114U);

  /* SLOW preset (50ms tap, 4x delay) */
  macro_player_set_speed(&mp, (uint8_t)MACRO_SPEED_SLOW);
  macro_player_trigger(&mp, cfg.macros, 0U, 200U);
  macro_player_step(&mp, cfg.macros, 200U, &rep);
  CHECK(mp.delay_until_ms == 250U);

  /* RGB OFF suppresses live macro typing glow */
  cfg.rgb_brightness = 0U;
  rgb_effects_render(&rctx, leds);
  CHECK(leds[a_led].r == 0U && leds[a_led].g == 0U && leds[a_led].b == 0U);

  /* TFT Review & Text Editor flow: Fn enters MACRO_REC_REVIEW when TFT active,
   * arrows/knob/Bksp/Del/typing edit inline, 2nd Fn confirms & saves, Knob Push cancels! */
  memset(cfg.macros, 0, sizeof(cfg.macros));
  macro_recorder_init(&rec);
  for (uint8_t i = 0U; i < HALL_SLOT_COUNT; i++) adc[i] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);

  ctx.tft_review_enabled = true;
  ctx.layer_state        = (1UL << 1U);
  ctx.now_ms             = 3000U;
  macro_key_event_t ev = { .macro_id = 0U, .pressed = true, .fn_active = true, .now_ms = 3000U };
  CHECK(!macro_recorder_on_macro_key(&rec, &ev));
  ctx.now_ms = 4850U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_STARTED);
  ctx.now_ms = 4900U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE && rec.state == MACRO_REC_RECORDING);

  /* Record "a" (3,1) then "s" (3,2) */
  adc[3U * HALL_COLS + 1U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5000U; (void)macro_recorder_tick(&rec, &ctx);
  adc[3U * HALL_COLS + 1U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5020U; (void)macro_recorder_tick(&rec, &ctx);
  adc[3U * HALL_COLS + 2U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5040U; (void)macro_recorder_tick(&rec, &ctx);
  adc[3U * HALL_COLS + 2U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5060U; (void)macro_recorder_tick(&rec, &ctx);

  /* First Fn tap enters TFT Review/Edit mode instead of saving immediately! */
  adc[5U * HALL_COLS + 10U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5100U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_REVIEW);
  CHECK(macro_recorder_is_reviewing(&rec) && rec.tok_count == 2U && rec.cursor == 2U);
  CHECK(rec.tokens[0].hid == KC_A && rec.tokens[1].hid == KC_S);

  /* Release Fn -> enters active MACRO_REC_REVIEW text editor */
  adc[5U * HALL_COLS + 10U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5120U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE && rec.state == MACRO_REC_REVIEW);

  /* Move cursor left by 1 via knob, insert Shift+'d' (row 3 col 3), then delete 's' via Del (row 0 col 13) */
  macro_recorder_editor_move(&rec, -1);
  CHECK(rec.cursor == 1U);
  adc[4U * HALL_COLS + 0U] = 1400U; /* LShift */
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5140U; (void)macro_recorder_tick(&rec, &ctx);
  adc[3U * HALL_COLS + 3U] = 1400U; /* D -> inserts Shift+D at pos 1 */
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5160U; (void)macro_recorder_tick(&rec, &ctx);
  adc[3U * HALL_COLS + 3U] = 2400U;
  adc[4U * HALL_COLS + 0U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5180U; (void)macro_recorder_tick(&rec, &ctx);
  CHECK(rec.tok_count == 3U && rec.cursor == 2U && rec.tokens[1].hid == KC_D && rec.tokens[1].mods == 0x02U);

  /* Press Backspace (row 1 col 13) -> deletes Shift+D at pos 1 */
  adc[1U * HALL_COLS + 13U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5200U; (void)macro_recorder_tick(&rec, &ctx);
  adc[1U * HALL_COLS + 13U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5220U; (void)macro_recorder_tick(&rec, &ctx);
  CHECK(rec.tok_count == 2U && rec.cursor == 1U && rec.tokens[0].hid == KC_A && rec.tokens[1].hid == KC_S);

  /* Insert 'd' at pos 1 -> result is "ads" */
  adc[3U * HALL_COLS + 3U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5240U; (void)macro_recorder_tick(&rec, &ctx);
  adc[3U * HALL_COLS + 3U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5260U; (void)macro_recorder_tick(&rec, &ctx);
  CHECK(rec.tok_count == 3U && rec.tokens[0].hid == KC_A && rec.tokens[1].hid == KC_D && rec.tokens[2].hid == KC_S);

  /* Second Fn tap confirms & serializes edited "ads" to VIA EEPROM */
  adc[5U * HALL_COLS + 10U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5300U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_SAVED);
  uint8_t const expect_ads[] = {
    0x01U, 0x01U, (uint8_t)KC_A,
    0x01U, 0x01U, (uint8_t)KC_D,
    0x01U, 0x01U, (uint8_t)KC_S,
    0x00U,
  };
  CHECK(memcmp(cfg.macros, expect_ads, sizeof(expect_ads)) == 0);
  adc[5U * HALL_COLS + 10U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 5320U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE && !macro_recorder_is_capturing(&rec));

  /* Knob push cancels Review mode cleanly without overwriting saved EEPROM slot */
  ev.macro_id = 0U; ev.pressed = true; ev.fn_active = true; ev.now_ms = 6000U;
  CHECK(!macro_recorder_on_macro_key(&rec, &ev));
  ctx.now_ms = 7850U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_STARTED);
  ctx.now_ms = 7900U; (void)macro_recorder_tick(&rec, &ctx);
  adc[5U * HALL_COLS + 10U] = 1400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 7950U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_REVIEW);
  adc[5U * HALL_COLS + 10U] = 2400U;
  for (int f = 0; f < 6; f++) hall_engine_process_frame(&hall, adc);
  ctx.now_ms = 7980U; (void)macro_recorder_tick(&rec, &ctx);
  CHECK(macro_recorder_editor_cancel(&rec, &hall) == MACRO_REC_EVT_CANCELLED);
  ctx.now_ms = 8000U;
  CHECK(macro_recorder_tick(&rec, &ctx) == MACRO_REC_EVT_NONE && !macro_recorder_is_capturing(&rec));
  CHECK(memcmp(cfg.macros, expect_ads, sizeof(expect_ads)) == 0); /* Original "ads" preserved! */

  /* Pixel HUD renders fullscreen TFT macro text editor cleanly and restores HOME on exit */
  tx_rec_t px_rec;
  memset(&px_rec, 0, sizeof(px_rec));
  pixel_hud_t px_ed;
  pixel_hud_init(&px_ed, &cfg, record_tx, &px_rec);
  uart7_rx_frame_t ver_rx = { .cmd = 0xA5U, .payload_len = 3U, .payload = { 0U, 1U, 2U } };
  uart7_rx_frame_t ack_rx = { .cmd = 0xA1U, .payload_len = 0U };
  pixel_hud_on_rx(&px_ed, &ver_rx);
  pixel_hud_live_t ed_live;
  memset(&ed_live, 0, sizeof(ed_live));
  macro_edit_tok_t sample_toks[3] = {
    { (uint8_t)KC_A, 0U }, { (uint8_t)KC_D, 0x02U }, { (uint8_t)KC_S, 0x01U }
  };
  ed_live.macro_reviewing   = true;
  ed_live.macro_rec_slot    = 0U;
  ed_live.macro_edit_toks   = sample_toks;
  ed_live.macro_edit_count  = 3U;
  ed_live.macro_edit_cursor = 1U;
  ed_live.macro_edit_rev    = 1U;
  pixel_hud_set_live(&px_ed, &ed_live);
  for (uint32_t t = 100U; t < 200U; t += 2U) {
    pixel_hud_tick(&px_ed, t);
    pixel_hud_on_rx(&px_ed, &ack_rx);
  }
  CHECK(px_ed.prev_macro_rev && px_ed.edit_dirty_mask == 0U && px_ed.q_count == 0U);
  ed_live.macro_reviewing = false;
  pixel_hud_set_live(&px_ed, &ed_live);
  for (uint32_t t = 202U; t < 260U; t += 2U) {
    pixel_hud_tick(&px_ed, t);
    pixel_hud_on_rx(&px_ed, &ack_rx);
  }
  CHECK(!px_ed.prev_macro_rev);
  return 0;
}

static int test_hall_calibration(void)
{
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);

  uint16_t rest[HALL_SLOT_COUNT], span[HALL_SLOT_COUNT], adc[HALL_SLOT_COUNT];
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    rest[i] = 2400U;
    span[i] = 1200U; /* Deliberately oversized span so 800-count switch only reaches ~2.6mm pre-cal */
    adc[i]  = 2400U;
  }
  hall_engine_t hall;
  hall_engine_init(&hall, rest, span);
  hall_engine_seed_idle_baseline(&hall, adc);

  /* 1. Start calibration via TFT menu HALL SWITCH -> Row 4 (CAL START) */
  tx_rec_t rec;
  memset(&rec, 0, sizeof(rec));
  pixel_hud_t px;
  pixel_hud_init(&px, &cfg, record_tx, &rec);
  uart7_rx_frame_t ver_rx = { .cmd = 0xA5U, .payload_len = 3U, .payload = { 0x00U, 1U, 2U } };
  pixel_hud_on_rx(&px, &ver_rx);
  pixel_hud_open_menu(&px, 100U);                  /* BRIGHTNESS row 1 */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_PREV, 110U)); /* row 0 (tab bar) */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 115U)); /* LIGHTING */
  CHECK(px.page == PIXEL_PAGE_LIGHTING);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 120U)); /* HALL SWITCH */
  CHECK(px.page == PIXEL_PAGE_SWITCH);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 130U));   /* row 1: ACT */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 140U)); /* row 2: RT */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 150U)); /* row 3: SOCD */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 160U)); /* row 4: CAL */
  CHECK(px.focus_row == 4U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 170U));   /* 1-click start */
  CHECK(px.cal_start_req && !px.menu_open);

  /* 2. Start hall_cal: counts all 82 active keys */
  hall_cal_t hcal;
  hall_cal_init(&hcal);
  hall_cal_start(&hcal, &hall);
  CHECK(hall_cal_is_active(&hcal) && hcal.total_keys == 82U && hcal.done_count == 0U);
  CHECK(hall_cal_step(&hcal, &hall, &cfg) == HALL_CAL_EVT_NONE && !hcal.wait_start_release);

  /* 3. Press 'A' (row 3, col 1) with an 800-count bottom-out -> shows "A", strength > 0, RGB changes */
  uint8_t slot_a = (uint8_t)(3U * HALL_COLS + 1U);
  adc[slot_a] = 1600U; /* 800-count drop */
  for (int f = 0; f < 20; f++) hall_engine_process_frame(&hall, adc);
  CHECK(hall_cal_step(&hcal, &hall, &cfg) == HALL_CAL_EVT_NONE);
  CHECK(strcmp(hcal.active_label, "A") == 0 && hcal.strength_pct > 50U && hcal.done_count == 1U);
  CHECK(hcal.done_mask[slot_a] == 1U);

  /* Verify RGB calibration guide: uncalibrated key=Blue, active key=Cyan-White, calibrated key=Mint */
  rgb_effects_state_t fx;
  memset(&fx, 0, sizeof(fx));
  rgb_grb_t leds[RGB_LED_COUNT];
  cfg.rgb_enabled    = 1U;
  cfg.rgb_brightness = 200U;
  rgb_render_ctx_t rctx = {
    .fx              = &fx,
    .cfg             = &cfg,
    .hall            = &hall,
    .cal_done_mask   = hcal.done_mask,
    .cal_active_slot = hcal.pressed_slot,
    .now_ms          = 200U,
  };
  rgb_effects_render(&rctx, leds);
  uint8_t a_led = rgb_matrix_to_led_idx(3U, 1U);
  uint8_t s_led = rgb_matrix_to_led_idx(3U, 2U);
  rgb_grb_t active_c = rgb_hsv_to_grb(128U, 60U, 200U);
  rgb_grb_t pend_c   = rgb_hsv_to_grb(160U, 255U, 200U);
  CHECK(leds[a_led].r == active_c.r && leds[a_led].g == active_c.g && leds[a_led].b == active_c.b);
  CHECK(leds[s_led].r == pend_c.r && leds[s_led].g == pend_c.g && leds[s_led].b == pend_c.b);

  /* Release A -> A turns Mint Green while still in calibration */
  adc[slot_a] = 2400U;
  for (int f = 0; f < 20; f++) hall_engine_process_frame(&hall, adc);
  CHECK(hall_cal_step(&hcal, &hall, &cfg) == HALL_CAL_EVT_NONE);
  rctx.cal_active_slot = hcal.pressed_slot;
  rgb_effects_render(&rctx, leds);
  rgb_grb_t done_c = rgb_hsv_to_grb(96U, 255U, 200U);
  CHECK(leds[a_led].r == done_c.r && leds[a_led].g == done_c.g && leds[a_led].b == done_c.b);

  /* 4. Press & release all remaining active keys -> auto-finishes & saves exact per-key spans */
  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) {
    if (hall.active_mask[i] != 0U) {
      adc[i] = 1600U;
    }
  }
  for (int f = 0; f < 20; f++) hall_engine_process_frame(&hall, adc);
  CHECK(hall_cal_step(&hcal, &hall, &cfg) == HALL_CAL_EVT_NONE);
  CHECK(hcal.done_count == 82U && hall_cal_is_active(&hcal)); /* Waits for release */

  for (uint32_t i = 0U; i < HALL_SLOT_COUNT; i++) adc[i] = 2400U;
  for (int f = 0; f < 20; f++) hall_engine_process_frame(&hall, adc);
  CHECK(hall_cal_step(&hcal, &hall, &cfg) == HALL_CAL_EVT_FINISHED);
  CHECK(!hall_cal_is_active(&hcal));
  CHECK(cfg.hall_user_span_adc[slot_a] >= 750U && cfg.hall_user_span_adc[slot_a] <= 800U);

  /* Now pressing 'A' to 1600 reaches full 4000um (4.0mm)! */
  adc[slot_a] = 1600U;
  for (int f = 0; f < 20; f++) hall_engine_process_frame(&hall, adc);
  CHECK(hall_engine_get_travel_um(&hall, 3U, 1U) == HALL_MAX_TRAVEL_UM);
  return 0;
}

static int test_caps_sidelight(void)
{
  tk75_persist_cfg_t cfg;
  tk75_cfg_set_defaults(&cfg);
  CHECK(cfg.caps_side_enabled == 1U && cfg.caps_side_effect == CAPS_SIDE_FX_BREATHING);

  hall_engine_t hall;
  memset(&hall, 0, sizeof(hall));
  rgb_effects_state_t fx;
  memset(&fx, 0, sizeof(fx));
  rgb_grb_t leds[RGB_LED_COUNT];

  /* 1. Normal sidelight OFF + brightness 0, Caps Lock ON -> LEDs 82..89 light up with fallback brightness */
  cfg.side_enabled      = 0U;
  cfg.side_brightness   = 0U;
  cfg.rgb_brightness    = 0U;
  cfg.caps_side_enabled = 1U;
  cfg.caps_side_effect  = CAPS_SIDE_FX_SOLID;
  rgb_render_ctx_t rctx = {
    .fx        = &fx,
    .cfg       = &cfg,
    .hall      = &hall,
    .host_leds = 0x02U,
    .now_ms    = 100U,
  };
  rgb_effects_render(&rctx, leds);
  rgb_grb_t solid_exp = rgb_hsv_to_grb(cfg.side_hue, cfg.side_sat, 160U);
  for (uint8_t i = 0U; i < RGB_SIDE_LED_COUNT; i++) {
    CHECK(leds[RGB_KEY_LED_COUNT + i].r == solid_exp.r &&
          leds[RGB_KEY_LED_COUNT + i].g == solid_exp.g &&
          leds[RGB_KEY_LED_COUNT + i].b == solid_exp.b);
  }

  /* 2. Alert Blink alternates ON (t=0) and OFF (t=250) */
  cfg.caps_side_effect = CAPS_SIDE_FX_BLINK;
  rctx.now_ms          = 0U;
  rgb_effects_render(&rctx, leds);
  CHECK((leds[RGB_KEY_LED_COUNT].r | leds[RGB_KEY_LED_COUNT].g | leds[RGB_KEY_LED_COUNT].b) > 0U);
  rctx.now_ms = 250U;
  rgb_effects_render(&rctx, leds);
  CHECK((leds[RGB_KEY_LED_COUNT].r | leds[RGB_KEY_LED_COUNT].g | leds[RGB_KEY_LED_COUNT].b) == 0U);

  /* 3. Scanner illuminates head LED brightly and leaves far LED dark */
  cfg.caps_side_effect = CAPS_SIDE_FX_SCANNER;
  rctx.now_ms          = 0U; /* scan_pos == 0 */
  rgb_effects_render(&rctx, leds);
  CHECK((leds[RGB_KEY_LED_COUNT + 0U].r | leds[RGB_KEY_LED_COUNT + 0U].g | leds[RGB_KEY_LED_COUNT + 0U].b) > 0U);
  CHECK((leds[RGB_KEY_LED_COUNT + 7U].r | leds[RGB_KEY_LED_COUNT + 7U].g | leds[RGB_KEY_LED_COUNT + 7U].b) == 0U);

  /* 4. Caps Lock OFF or caps_side_enabled == 0 keeps sidelight OFF when side_enabled == 0 */
  rctx.host_leds = 0x00U;
  rgb_effects_render(&rctx, leds);
  CHECK((leds[RGB_KEY_LED_COUNT].r | leds[RGB_KEY_LED_COUNT].g | leds[RGB_KEY_LED_COUNT].b) == 0U);
  rctx.host_leds        = 0x02U;
  cfg.caps_side_enabled = 0U;
  rgb_effects_render(&rctx, leds);
  CHECK((leds[RGB_KEY_LED_COUNT].r | leds[RGB_KEY_LED_COUNT].g | leds[RGB_KEY_LED_COUNT].b) == 0U);

  /* 5. VIA ch=6 val_id=8 (enable) and val_id=9 (effect) SET/GET */
  via_context_t via;
  memset(&via, 0, sizeof(via));
  via.cfg = &cfg;
  uint8_t req[VIA_RAW_EPSIZE] = { 0x07U, 6U, 8U, 1U };
  uint8_t rep[VIA_RAW_EPSIZE] = { 0 };
  via_process_packet(&via, req, rep);
  CHECK(cfg.caps_side_enabled == 1U);
  req[2] = 9U; req[3] = 3U; /* Alert Blink */
  via_process_packet(&via, req, rep);
  CHECK(cfg.caps_side_effect == 3U);
  req[0] = 0x08U;
  via_process_packet(&via, req, rep);
  CHECK(rep[3] == 3U);
  req[0] = 0x07U; req[3] = 99U; /* Out-of-range clamps to 0 */
  via_process_packet(&via, req, rep);
  CHECK(cfg.caps_side_effect == 0U);

  /* 6. TFT BRIGHTNESS page row 4 (CAP) cycles OFF -> SOLID -> BREATH -> ... -> SCAN -> OFF */
  tx_rec_t rec;
  memset(&rec, 0, sizeof(rec));
  pixel_hud_t px;
  pixel_hud_init(&px, &cfg, record_tx, &rec);
  uart7_rx_frame_t ver_rx = { .cmd = 0xA5U, .payload_len = 3U, .payload = { 0x00U, 1U, 2U } };
  pixel_hud_on_rx(&px, &ver_rx);
  cfg.caps_side_enabled = 0U;
  pixel_hud_open_menu(&px, 100U);                        /* BRIGHTNESS row 1 */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 110U)); /* row 2: RGB */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 120U)); /* row 3: SID */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 130U)); /* row 4: CAP */
  CHECK(px.focus_row == 4U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 140U));   /* Enter edit */
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 150U));  /* OFF -> SOLID */
  CHECK(cfg.caps_side_enabled == 1U && cfg.caps_side_effect == CAPS_SIDE_FX_SOLID);
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 160U));  /* SOLID -> BREATH */
  CHECK(cfg.caps_side_enabled == 1U && cfg.caps_side_effect == CAPS_SIDE_FX_BREATHING);
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_PREV, 170U));  /* BREATH -> SOLID */
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_PREV, 180U));  /* SOLID -> OFF */
  CHECK(cfg.caps_side_enabled == 0U);

  /* 7. TFT LIGHTING page edits FX, COL, SFX, and SCOL cleanly */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 190U));   /* exit edit */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 195U)); /* wrap row 4 -> row 0 */
  CHECK(px.focus_row == 0U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 200U)); /* BRIGHTNESS -> LIGHTING */
  CHECK(px.page == PIXEL_PAGE_LIGHTING);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 205U));   /* row 1: FX */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 210U));   /* edit FX */
  uint8_t prev_fx = cfg.rgb_effect;
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 215U));
  CHECK(cfg.rgb_effect == (uint8_t)((prev_fx + 1U) % 7U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 220U));   /* exit FX edit */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 225U)); /* row 2: COL */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 230U));   /* edit COL */
  cfg.rgb_hue = 0U; cfg.rgb_sat = 255U;                  /* RED */
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 235U));  /* -> ORN (hue=22) */
  CHECK(cfg.rgb_hue == 22U && cfg.rgb_sat == 255U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 240U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 245U)); /* row 3: SFX */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 250U));   /* edit SFX */
  cfg.side_enabled = 0U; cfg.side_brightness = 0U;
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 255U));  /* OFF -> WAVE + auto brightness */
  CHECK(cfg.side_enabled == 1U && cfg.side_effect == 1U && cfg.side_brightness > 0U);
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 260U));
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_NEXT, 265U)); /* row 4: SCOL */
  CHECK(!pixel_hud_input(&px, HUD_MENU_ACT_OK, 270U));
  cfg.side_hue = 0U; cfg.side_sat = 255U;                /* RED */
  CHECK(pixel_hud_input(&px, HUD_MENU_ACT_PREV, 275U));  /* wrap -> WHT (sat=0) */
  CHECK(cfg.side_sat == 0U);
  return 0;
}

int main(void)
{
  if (test_factory_cal() != 0) return 1;
  if (test_flash_eeprom() != 0) return 1;
  if (test_hall_rt_and_socd() != 0) return 1;
  if (test_encoder_and_keymap_macros() != 0) return 1;
  if (test_via_and_rgb() != 0) return 1;
  if (test_uart7_and_hud() != 0) return 1;
  if (test_spi3_rf_and_wireless() != 0) return 1;
  if (test_review_hardening() != 0) return 1;
  if (test_battery_curve_and_gauge() != 0) return 1;
  if (test_tft_menu_navigation() != 0) return 1;
  if (test_pixel_hud() != 0) return 1;
  if (test_xtrfy_macro_recorder() != 0) return 1;
  if (test_hall_calibration() != 0) return 1;
  if (test_caps_sidelight() != 0) return 1;

  printf("ALL_C_TESTS_PASSED checks=%u\n", g_checks);
  return 0;
}
