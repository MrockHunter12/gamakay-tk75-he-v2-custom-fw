#ifndef VIA_PROTOCOL_H
#define VIA_PROTOCOL_H

#include "flash_eeprom.h"
#include "hall_engine.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIA_PROTOCOL_VERSION  0x000CU /* VIA v12 */
#define VIA_RAW_EPSIZE        32U

typedef enum {
  VIA_EVT_NONE = 0,
  VIA_EVT_SAVE_EEPROM,
  VIA_EVT_RESET_EEPROM,
  VIA_EVT_BOOTLOADER_JUMP,
  VIA_EVT_HALL_CONFIG_CHANGED,
  VIA_EVT_RGB_CHANGED,
  VIA_EVT_TFT_CHANGED,
  VIA_EVT_RECALIBRATE_IDLE,
  VIA_EVT_RTC_SYNC,
  VIA_EVT_TFT_MEDIA_CHUNK,
  VIA_EVT_RF_CONFIG_CHANGED,
  VIA_EVT_RF_PAIR,
  VIA_EVT_TFT_RAW,
} via_event_type_t;

typedef struct {
  uint16_t year;
  uint8_t  month;
  uint8_t  day;
  uint8_t  hour;
  uint8_t  min;
  uint8_t  sec;
} via_rtc_time_t;

typedef struct {
  uint8_t  flags; /* bit0: first chunk, bit1: last chunk */
  uint16_t seq;
  uint8_t  len;
  uint8_t  data[24];
} via_media_chunk_t;

typedef struct {
  uint8_t op;     /* 0=pause, 1=stage, 2=send, 3=stage+send, 4=clear_rx */
  uint8_t cmd;
  uint8_t offset;
  uint8_t len;
  uint8_t data[24];
} via_tft_raw_t;

typedef struct {
  uint8_t  conn_mode;
  uint8_t  bt_slot;
  uint8_t  rf_state;
  uint8_t  battery_pct;
  uint8_t  vbus_present;
  uint8_t  charging;
  uint8_t  asleep;
  uint8_t  rf_link_up;
  uint16_t tft_rx_count;
  uint8_t  tft_last_cmd;
  uint8_t  tft_last_len;
  uint8_t  tft_last_payload[16];
  uint16_t hall_min_adc;
  uint16_t hall_max_adc;
  uint16_t pressed_count;
  uint8_t  cal_valid_count;
  uint16_t vbat_adc;
  uint16_t tft_menu_cursor;
  uint8_t  tft_menu_pic_slot;
  uint8_t  enc_raw_bits;
  uint8_t  tft_info_len;
  uint8_t  tft_info[24];
  uint8_t  tft_raw_rx_ready;
  uint8_t  tft_raw_rx_cmd;
  uint8_t  tft_raw_rx_len;
  uint8_t  tft_raw_rx_payload[24];
} via_diag_t;

typedef struct {
  via_event_type_t type;
  via_rtc_time_t   rtc;
  via_media_chunk_t media;
  via_tft_raw_t     tft_raw;
} via_event_t;

typedef struct {
  tk75_persist_cfg_t *cfg;
  hall_engine_t const *hall;
  via_diag_t const   *diag;
  uint32_t             uptime_ms;
  void               (*on_event)(via_event_t const *evt, void *ctx);
  void                *event_ctx;
} via_context_t;

/** Process a 32-byte VIA command packet and write 32-byte response into out_reply. */
void via_process_packet(via_context_t *via, uint8_t const in_req[VIA_RAW_EPSIZE], uint8_t out_reply[VIA_RAW_EPSIZE]);

#ifdef __cplusplus
}
#endif

#endif /* VIA_PROTOCOL_H */
