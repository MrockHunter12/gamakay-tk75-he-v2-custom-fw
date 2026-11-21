#ifndef HUD_ENGINE_H
#define HUD_ENGINE_H

#include "flash_eeprom.h"
#include "uart7_tft.h"
#include "via_protocol.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HUD_CMD_STATUS      0x01U
#define HUD_CMD_OPTIONS     0x02U
#define HUD_CMD_MEDIA_PREP  0x03U
#define HUD_CMD_MEDIA       0x04U
#define HUD_CMD_MEDIA_ERASE 0x06U
#define HUD_CMD_HEARTBEAT   0x07U
#define HUD_CMD_WAKE_READY  0x08U /* Read-only TFT version/ID query (0x06 is full flash erase!) */
#define HUD_CMD_RTC         0x09U

#define HUD_WPM_BUCKETS    10U
#define HUD_MAX_PIC_SLOTS  6U
#define HUD_MENU_TIMEOUT_MS 15000U

typedef enum {
  HUD_MENU_ACT_NONE = 0,
  HUD_MENU_ACT_PREV = 1,
  HUD_MENU_ACT_NEXT = 2,
  HUD_MENU_ACT_OK   = 3,
} hud_menu_act_t;

typedef enum {
  HUD_MENU_HOME         = 0x0000U,
  HUD_MENU_PIC_MENU     = 0x0001U,
  HUD_MENU_PIC_VIEW     = 0x0002U,
  HUD_MENU_SIDE_ROOT    = 0x0003U,
  HUD_MENU_RGB_ROOT     = 0x0004U,
  HUD_MENU_OS_ROOT      = 0x0005U,
  HUD_MENU_LANG_ROOT    = 0x0006U,
  HUD_MENU_INFO_ROOT    = 0x0007U,
  HUD_MENU_PIC_SUB      = 0x0011U,
  HUD_MENU_SIDE_FX      = 0x0013U,
  HUD_MENU_SIDE_COLOR   = 0x0023U,
  HUD_MENU_SIDE_BRIGHT  = 0x0033U,
  HUD_MENU_SIDE_SPEED   = 0x0043U,
  HUD_MENU_SIDE_BACK    = 0x0053U,
  HUD_MENU_SIDE_ED_FX   = 0x0113U,
  HUD_MENU_SIDE_ED_CLR  = 0x0123U,
  HUD_MENU_SIDE_ED_BRT  = 0x0133U,
  HUD_MENU_SIDE_ED_SPD  = 0x0143U,
  HUD_MENU_RGB_FX       = 0x0014U,
  HUD_MENU_RGB_COLOR    = 0x0024U,
  HUD_MENU_RGB_BRIGHT   = 0x0034U,
  HUD_MENU_RGB_SPEED    = 0x0044U,
  HUD_MENU_RGB_BACK     = 0x0054U,
  HUD_MENU_RGB_ED_FX    = 0x0114U,
  HUD_MENU_RGB_ED_CLR   = 0x0124U,
  HUD_MENU_RGB_ED_BRT   = 0x0134U,
  HUD_MENU_RGB_ED_SPD   = 0x0144U,
  HUD_MENU_OS           = 0x0015U,
  HUD_MENU_LANG         = 0x0016U,
  HUD_MENU_INFO         = 0x0017U,
} hud_menu_cursor_t;

typedef bool (*hud_send_frame_fn_t)(uart7_tx_frame_t const *frame, void *ctx);

typedef struct {
  tk75_persist_cfg_t *cfg;
  hud_send_frame_fn_t send_fn;
  void               *send_ctx;
} hud_init_cfg_t;

typedef struct {
  uint8_t active_layer;
  uint8_t battery_pct;
  uint8_t host_leds;
  bool    mac_mode;
  uint8_t conn_mode; /* 0=USB Wired, 1=2.4GHz, 2=Bluetooth */
  uint8_t bt_slot;   /* 0=BT1, 1=BT2, 2=BT3 */
  uint8_t rf_state;     /* 0=disconn, 1=pairing, 2=reconnecting, 3=connected, 4=sleep */
  bool    charging;     /* Active battery charging */
  bool    vbus_present; /* USB VBUS present */
} hud_status_t;

typedef struct {
  tk75_persist_cfg_t *cfg;
  hud_send_frame_fn_t send_fn;
  void               *send_ctx;
  uint32_t            last_status_ms;
  uint32_t            last_heartbeat_ms;
  uint32_t            last_rtc_tick_ms;
  uint32_t            last_bucket_ms;
  uint16_t            wpm_buckets[HUD_WPM_BUCKETS];
  uint8_t             wpm_bucket_idx;
  uint16_t            wpm_current;
  hud_status_t        status;
  via_rtc_time_t      rtc;
  bool                rtc_valid;
  bool                rtc_tick_primed;
  bool                status_dirty;
  bool                options_dirty;
  bool                menu_dirty;
  bool                rtc_dirty;
  bool                wake_ready_sent;
  uint16_t            menu_cursor;
  uint16_t            menu_return_cursor;
  uint8_t             menu_pic_slot;
  uint8_t             menu_return_pic_slot;
  uint8_t             menu_trigger;
  uint8_t             menu_lang_dirty;
  uint8_t             menu_tft_brt_dn;
  uint8_t             menu_tft_brt_up;
  uint8_t             sent_tft_brt_dn;
  uint8_t             sent_tft_brt_up;
  uint8_t             menu_sync_ticks;
  uint8_t             menu_pic_exists[HUD_MAX_PIC_SLOTS];
  uint32_t            menu_last_input_ms;
  uint8_t             media_slot;
  uint8_t             media_frame_idx;
  uint8_t             media_total_frames;
  uint8_t             media_delay_units;
  uint16_t            media_chunk_idx;
  uint8_t             media_buf[64];
  uint8_t             media_fill;
  uint16_t            rx_count;
  uint8_t             last_rx_cmd;
  uint8_t             last_rx_len;
  uint8_t             last_rx_payload[16];
  uint8_t             info_len;
  uint8_t             info_payload[24];
} hud_engine_t;

/** Initialize HUD sync engine with config storage and frame transport callback. */
void hud_engine_init(hud_engine_t *hud, hud_init_cfg_t const *init);

/** Record one key press event for real-time WPM telemetry. */
void hud_engine_note_keystroke(hud_engine_t *hud, uint32_t now_ms);

/** Advance WPM sliding buckets without transmitting stock UART7 frames. */
void hud_engine_update_wpm(hud_engine_t *hud, uint32_t now_ms);

/** Update keyboard telemetry fields; marks status dirty when values change. */
void hud_engine_set_status(hud_engine_t *hud, hud_status_t const *st);

/** Mark display/TFT options as changed (sends cmd 0x02 on next tick). */
void hud_engine_mark_options_dirty(hud_engine_t *hud);

/** Set wall-clock RTC from VIA host sync and schedule UART7 cmd 0x09. */
void hud_engine_set_rtc(hud_engine_t *hud, via_rtc_time_t const *rtc);

/** Forward one custom image/GIF chunk (via VIA Ext val_id=3) to TFT module over cmd 0x04. */
bool hud_engine_push_media_chunk(hud_engine_t *hud, via_media_chunk_t const *chunk);

/** Apply an incoming TFT -> STM32 UI sync frame ('D' cmd 0x81/0x82); returns true if cfg changed. */
bool hud_engine_handle_rx(hud_engine_t *hud, uart7_rx_frame_t const *frame);

/** Return true while TFT cursor is inside an interactive top menu or submenu. */
bool hud_engine_is_menu_open(hud_engine_t const *hud);

/** Exit any open TFT menu back to Home or the previously viewed picture slot. */
void hud_engine_close_menu(hud_engine_t *hud);

/** Apply a TFT menu navigation action (Prev/Next/OK); returns true if persistent cfg changed. */
bool hud_engine_menu_input(hud_engine_t *hud, hud_menu_act_t act, uint32_t now_ms);

/** Advance timers and flush at most one queued UART7 packet per tick. */
void hud_engine_tick(hud_engine_t *hud, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif /* HUD_ENGINE_H */
