#ifndef FLASH_EEPROM_H
#define FLASH_EEPROM_H

#include "hall_types.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TK75_CFG_MAGIC        0x544B3735UL /* "TK75" */
#define TK75_CFG_VERSION      3U
#define TK75_KEYMAP_LAYERS    4U
#define TK75_KEYMAP_ROWS      6U
#define TK75_KEYMAP_COLS      16U
#define TK75_MACRO_COUNT      16U
#define TK75_MACRO_BUF_BYTES  2048U
#define TK75_EEPROM_SLOTS     4U

typedef struct {
  uint32_t magic;
  uint32_t seq;
  uint16_t version;
  uint16_t layout_options; /* bit 0: 1 = ISO, 0 = ANSI */

  uint16_t keymap[TK75_KEYMAP_LAYERS][TK75_KEYMAP_ROWS][TK75_KEYMAP_COLS];
  uint16_t encoders[TK75_KEYMAP_LAYERS][2]; /* [0]=CCW, [1]=CW */
  uint8_t  macros[TK75_MACRO_BUF_BYTES];

  /* Hall Effect settings */
  uint16_t hall_actuation_um;
  uint16_t hall_hyst_um;
  uint8_t  hall_rt_enabled;
  uint8_t  hall_rt_continuous;
  uint16_t hall_rt_down_um;
  uint16_t hall_rt_up_um;
  uint8_t  hall_socd_mode;
  uint8_t  hall_socd_preset;
  uint8_t  hall_per_key_act_step[HALL_SLOT_COUNT]; /* 0 = use global, else step * 50 um */
  uint8_t  hall_per_key_rt[HALL_SLOT_COUNT];       /* bit0: override, bit1: en, bits2..7: step*50um */

  /* Per-key Matrix RGB & Sidelight settings */
  uint8_t  rgb_enabled;
  uint8_t  rgb_brightness;
  uint8_t  rgb_effect;
  uint8_t  rgb_speed;
  uint8_t  rgb_hue;
  uint8_t  rgb_sat;
  uint8_t  caps_ind_enabled;

  uint8_t  side_enabled;
  uint8_t  side_brightness;
  uint8_t  side_effect;
  uint8_t  side_speed;
  uint8_t  side_hue;
  uint8_t  side_sat;

  /* TFT Screen & Encoder settings */
  uint8_t  tft_enabled;
  uint8_t  tft_page;
  uint8_t  tft_brightness;
  uint8_t  tft_timeout_idx;
  uint8_t  tft_clock_24h;
  uint8_t  tft_wpm_enabled;
  uint8_t  tft_he_bar_enabled;
  uint8_t  encoder_mode; /* 0=Keymap/Volume, 1=TFT Page, 2=RGB Bright, 3=Actuation Tune */
  uint8_t  win_lock;
  uint8_t  rf_bt_slot;        /* 0=BT1, 1=BT2, 2=BT3 */
  uint8_t  rf_poll_rate_idx;  /* 0=1000Hz, 1=500Hz, 2=250Hz, 3=125Hz */
  uint8_t  mac_mode;          /* 0=Win/Linux, 1=macOS */
  uint8_t  sleep_timeout_min; /* Battery idle sleep minutes (0=disabled, default 5) */
  uint8_t  macro_speed_idx;   /* 1=FAST(2ms/0.25x), 2/0=NORM(10ms/1x), 3=MED(25ms/2x), 4=SLOW(50ms/4x), 5=SAFE(100ms/8x) */
  uint8_t  tft_screensaver;   /* 0=Off, 1=Pixel-art idle clock & system info screensaver */
  uint8_t  caps_side_enabled; /* 0=Off, 1=Trigger sidelight ON while Caps Lock active */
  uint8_t  caps_side_effect;  /* 0=Solid, 1=Breathing, 2=Rainbow Wave, 3=Alert Blink, 4=Scanner */
  uint8_t  reserved[2];
  uint16_t hall_user_span_adc[HALL_SLOT_COUNT]; /* 0=factory cal, else user-calibrated full-travel ADC span */

  uint32_t crc32;
} tk75_persist_cfg_t;

_Static_assert(sizeof(tk75_persist_cfg_t) <= 4096U, "Persist config must fit in one 4KB EEPROM slot");
_Static_assert((sizeof(tk75_persist_cfg_t) & 3U) == 0U, "Persist config size must be 4-byte aligned");
_Static_assert(offsetof(tk75_persist_cfg_t, crc32) == 3248U, "Persist config must have no implicit holes");

/** Abstracted flash backend (hardware flash on AT32, RAM buffer in host tests). */
typedef struct {
  bool (*read_slot)(uint8_t slot_idx, void *dst, uint32_t len, void *ctx);
  bool (*erase_slot)(uint8_t slot_idx, void *ctx);
  bool (*write_slot)(uint8_t slot_idx, void const *src, uint32_t len, void *ctx);
  void *ctx;
} flash_eeprom_backend_t;

/** Populate cfg with compiled ISO defaults (keeps keymap/RGB/Hall/TFT sane). */
void tk75_cfg_set_defaults(tk75_persist_cfg_t *cfg);

/** Compute IEEE CRC32 over config payload excluding trailing crc32 field. */
uint32_t tk75_cfg_crc32(tk75_persist_cfg_t const *cfg);

/** Load newest valid config across 4 wear-leveled slots; sets defaults if empty. Returns active slot. */
uint8_t flash_eeprom_load(flash_eeprom_backend_t const *be, tk75_persist_cfg_t *out_cfg);

/** Write config to next round-robin wear-leveled slot with incremented seq and CRC32. */
bool flash_eeprom_save(flash_eeprom_backend_t const *be, tk75_persist_cfg_t *cfg, uint8_t *io_active_slot);

/** Erase all 4 EEPROM slots cleanly (factory reset). */
bool flash_eeprom_erase_all(flash_eeprom_backend_t const *be);

#ifdef AT32F405RCT7
/** Return hardware AT32 flash backend bound to 0x0803C000..0x0803FFFF (4 x 4KB slots). */
flash_eeprom_backend_t const *flash_eeprom_at32_backend(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* FLASH_EEPROM_H */
