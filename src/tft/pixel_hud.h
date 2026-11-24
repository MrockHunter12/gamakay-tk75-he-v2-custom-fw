#ifndef PIXEL_HUD_H
#define PIXEL_HUD_H

#include "flash_eeprom.h"
#include "hud_engine.h"
#include "macro_recorder.h"
#include "uart7_tft.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PIXEL_HUD_CMD_MANUAL     0x20U
#define PIXEL_HUD_CMD_FILL_RECT  0x21U
#define PIXEL_HUD_CMD_BLIT       0x22U
#define PIXEL_HUD_CMD_TEXT       0x23U
#define PIXEL_HUD_CMD_BRIGHTNESS 0x24U
#define PIXEL_HUD_CMD_VERSION    0x25U
#define PIXEL_HUD_CMD_BATCH_RECT 0x26U
#define PIXEL_HUD_CMD_U9_FLASH   0x28U
#define PIXEL_HUD_CMD_GIF_PLAY   0x29U

#define PIXEL_HUD_QUEUE_CAP      48U

typedef enum {
  PIXEL_PAGE_HOME = 0,
  PIXEL_PAGE_BRIGHTNESS,
  PIXEL_PAGE_LIGHTING,
  PIXEL_PAGE_SWITCH,
  PIXEL_PAGE_MACRO,
  PIXEL_PAGE_POWER,
  PIXEL_PAGE_SYSTEM,
  PIXEL_PAGE_COUNT,
} pixel_hud_page_t;

typedef struct {
  uint8_t  active_layer;
  uint8_t  battery_pct;
  uint8_t  host_leds;
  uint8_t  conn_mode;    /* 0=USB, 1=2.4G, 2=BT */
  uint8_t  bt_slot;      /* 0..2 */
  bool     charging;
  bool     vbus_present;
  bool     mac_mode;
  uint16_t wpm;
  uint16_t max_travel_um; /* Deepest currently pressed Hall key (0..4000 um) */
  bool     any_actuated;  /* True when deepest key is logically actuated */
  bool                    macro_recording;
  bool                    macro_reviewing;
  uint8_t                 macro_rec_slot;
  uint16_t                macro_rec_bytes;   /* Current recorded VIA bytecode length (0..MACRO_REC_MAX_BYTES) */
  macro_edit_tok_t const *macro_edit_toks;   /* Active token buffer during TFT macro text editor */
  uint16_t                macro_edit_count;
  uint16_t                macro_edit_cursor;
  uint16_t                macro_edit_rev;
  uint8_t                 macro_speed_toast; /* 0=none, 1..5=show M:FAST..M:SAFE in header */
  bool     rtc_valid;
  uint8_t  rtc_hour;
  uint8_t  rtc_min;
  uint8_t  rtc_sec;
  uint32_t uptime_sec;
  uint32_t idle_ms;
  uint16_t peak_wpm;
  uint32_t session_keys;
  bool     cal_active;
  uint8_t  cal_done_count;
  uint8_t  cal_total_keys;
  uint8_t  cal_strength_pct;
  bool     cal_key_done;
  char     cal_label[5];
} pixel_hud_live_t;

typedef struct {
  uint8_t cmd;
  uint8_t len;
  uint8_t data[UART7_MAX_PAYLOAD];
} pixel_hud_op_t;

typedef struct {
  tk75_persist_cfg_t *cfg;
  hud_send_frame_fn_t send_fn;
  void               *send_ctx;

  bool                detected;       /* True once U24 replies 0xA5 (patch v1.1+) */
  uint8_t             patch_major;
  uint8_t             patch_minor;
  uint8_t             applied_theme;  /* Currently rendered tft_theme_t */
  bool                menu_open;
  uint8_t             page;           /* pixel_hud_page_t */
  uint8_t             focus_row;      /* 0=page tab bar, 1..4=items */
  bool                editing;        /* True while knob adjusts focused item value */

  uint32_t            last_probe_ms;
  uint32_t            last_tx_ms;
  uint32_t            last_anim_ms;
  uint32_t            last_input_ms;
  bool                awaiting_ack;

  /* Live telemetry & smoothed switch animation state */
  pixel_hud_live_t    live;
  uint8_t             anim_plunger_px;  /* Current rendered plunger travel (0..18 px) */
  bool                prev_actuated;
  uint16_t            prev_wpm;
  uint16_t            prev_travel_disp; /* Displayed 0.1 mm units */
  uint16_t            smooth_travel_um; /* Filtered display travel (anti-jitter) */
  bool                travel_active;    /* Hysteresis latch for live travel display */
  uint32_t            prev_header_hash;
  uint8_t             prev_badges_hash;
  uint8_t             prev_home_hour;
  uint8_t             prev_home_min;
  uint8_t             prev_home_sec_odd;
  bool                prev_home_rtc_valid;
  uint8_t             applied_tft_brt;
  uint8_t             brt_reapply_left;
  bool                brt_reapply_armed;
  uint32_t            brt_reapply_ms;
  bool                full_redraw_needed;
  bool                body_redraw_needed;
  bool                tft_brt_dirty;
  uint8_t             macro_sel_slot; /* 0..4 (C, V, B, N, M) selected in MACRO->KEYS row */
  uint8_t             macro_play_req; /* 0xFF=none, 0..4=slot requested to play */
  bool                cal_start_req;  /* Set when user triggers HALL->CAL START */
  bool                prev_macro_rec;
  bool                prev_macro_rev;
  uint8_t             rec_anim_phase;
  uint16_t            prev_rec_bytes;
  uint16_t            prev_edit_rev;
  uint16_t            prev_edit_cursor;
  uint8_t             edit_scroll_row;
  uint8_t             edit_dirty_mask; /* bits 0..4=rows, bit 5=header/inspector/scrollbar */
  bool                edit_blink_on;
  bool                prev_cal_active;
  uint8_t             prev_cal_done;
  uint8_t             prev_cal_pct;
  bool                prev_cal_key_done;
  char                prev_cal_label[5];
  bool                screensaver_active;
  uint8_t             saver_mode_rendered;
  uint8_t             saver_phase;
  uint16_t            saver_step;
  uint8_t             prev_saver_hour;
  uint8_t             prev_saver_min;
  uint8_t             prev_saver_sec;
  bool                prev_saver_rtc_valid;
  uint32_t            prev_saver_hash;
  int16_t             dvd_x;
  int16_t             dvd_y;
  int8_t              dvd_dx;
  int8_t              dvd_dy;
  uint8_t             dvd_col_idx;
  uint8_t             dvd_corners;
  uint8_t             matrix_y[10];
  bool                u9_gif_probed;
  bool                u9_gif_valid;
  bool                u9_gif_fallback_drawn;
  uint8_t             u9_gif_frames;
  uint8_t             u9_gif_cur_frame;
  uint16_t            u9_gif_delay_ms;
  uint32_t            u9_gif_probe_ms;

  /* Non-blocking UART7 draw command ring buffer */
  pixel_hud_op_t      q[PIXEL_HUD_QUEUE_CAP];
  uint8_t             q_head;
  uint8_t             q_tail;
  uint8_t             q_count;
} pixel_hud_t;

/** Initialize multi-theme pixel HUD engine bound to persistent config and UART7 sender. */
void pixel_hud_init(pixel_hud_t *hud, tk75_persist_cfg_t *cfg,
                    hud_send_frame_fn_t send_fn, void *send_ctx);

/** Returns true when patched U24 is detected and Pixel HUD owns the display. */
bool pixel_hud_is_active(pixel_hud_t const *hud);

/** Returns true when interactive Pixel HUD menu is currently open. */
bool pixel_hud_is_menu_open(pixel_hud_t const *hud);

/** Return normalized active theme ID (0=Pixel, 1=Stock+, 2=Terminal). */
uint8_t pixel_hud_get_theme(pixel_hud_t const *hud);

/** Feed incoming UART7 'D' frame; handles patch detection (0xA5) and ACK pacing. */
void pixel_hud_on_rx(pixel_hud_t *hud, uart7_rx_frame_t const *frame);

/** Update live Hall travel, WPM, battery, mode, and LED status. */
void pixel_hud_set_live(pixel_hud_t *hud, pixel_hud_live_t const *live);

/** Notify that config (theme/brightness/RGB/Hall/Power/OS) changed externally (e.g. VIA). */
void pixel_hud_mark_config_dirty(pixel_hud_t *hud);

/** Open interactive menu (jumps cleanly to Brightness page, row 1 ready). */
void pixel_hud_open_menu(pixel_hud_t *hud, uint32_t now_ms);

/** Close menu and return immediately to Home dashboard (no page animation). */
void pixel_hud_close_menu(pixel_hud_t *hud);

/** Handle knob action (PREV/NEXT/OK); returns true if persistent cfg changed. */
bool pixel_hud_input(pixel_hud_t *hud, hud_menu_act_t act, uint32_t now_ms);

/** Advance probe, animations, timeouts, and flush queued UART7 commands. */
void pixel_hud_tick(pixel_hud_t *hud, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* PIXEL_HUD_H */
