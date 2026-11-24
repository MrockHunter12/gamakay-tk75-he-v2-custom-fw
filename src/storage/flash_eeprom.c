#include "flash_eeprom.h"
#include "default_keymap_iso.h"
#include <stddef.h>
#include <string.h>

#ifdef AT32F405RCT7
#include "at32f402_405.h"
#include "board_tk75.h"
#include "tk75_pins.h"
#endif

static uint32_t crc32_update(uint32_t crc, uint8_t byte)
{
  crc ^= byte;
  for (uint8_t i = 0U; i < 8U; i++) {
    uint32_t mask = (uint32_t)(-(int32_t)(crc & 1U));
    crc = (crc >> 1U) ^ (0xEDB88320UL & mask);
  }
  return crc;
}

uint32_t tk75_cfg_crc32(tk75_persist_cfg_t const *cfg)
{
  uint8_t const *raw = (uint8_t const *)cfg;
  size_t len = offsetof(tk75_persist_cfg_t, crc32);
  uint32_t crc = 0xFFFFFFFFUL;

  for (size_t i = 0U; i < len; i++) {
    crc = crc32_update(crc, raw[i]);
  }
  return ~crc;
}

void tk75_cfg_set_defaults(tk75_persist_cfg_t *cfg)
{
  memset(cfg, 0, sizeof(*cfg));
  cfg->magic          = TK75_CFG_MAGIC;
  cfg->seq            = 1U;
  cfg->version        = TK75_CFG_VERSION;
  cfg->layout_options = 0x0001U; /* bit0 = ISO */

  keymap_copy_default_layers(cfg->keymap);
  keymap_copy_default_encoders(cfg->encoders);

  cfg->hall_actuation_um  = HALL_DEFAULT_ACT_UM;
  cfg->hall_hyst_um       = HALL_DEFAULT_HYST_UM;
  cfg->hall_rt_enabled    = 0U;
  cfg->hall_rt_continuous = 0U;
  cfg->hall_rt_down_um    = HALL_DEFAULT_RT_DOWN_UM;
  cfg->hall_rt_up_um      = HALL_DEFAULT_RT_UP_UM;
  cfg->hall_socd_mode     = HALL_SOCD_OFF;
  cfg->hall_socd_preset   = 0U;

  cfg->rgb_enabled        = 1U;
  cfg->rgb_brightness     = 160U;
  cfg->rgb_effect         = 1U; /* Rainbow wave default */
  cfg->rgb_speed          = 128U;
  cfg->rgb_hue            = 128U;
  cfg->rgb_sat            = 255U;
  cfg->caps_ind_enabled   = 1U;

  cfg->side_enabled       = 1U;
  cfg->side_brightness    = 160U;
  cfg->side_effect        = 1U;
  cfg->side_speed         = 128U;
  cfg->side_hue           = 160U;
  cfg->side_sat           = 255U;

  cfg->tft_enabled        = 1U;
  cfg->tft_page           = 0U;
  cfg->tft_brightness     = 80U;
  cfg->tft_timeout_idx    = 2U; /* 5 min */
  cfg->tft_clock_24h      = 1U;
  cfg->tft_wpm_enabled    = 1U;
  cfg->tft_he_bar_enabled = 1U;
  cfg->encoder_mode       = 0U;
  cfg->win_lock           = 0U;
  cfg->rf_bt_slot         = 0U;
  cfg->rf_poll_rate_idx   = 0U;
  cfg->mac_mode           = 0U;
  cfg->sleep_timeout_min  = 5U;
  cfg->macro_speed_idx    = 2U; /* NORM (10ms tap / 1x delay) */
  cfg->tft_screensaver    = 1U; /* Pixel-art idle screensaver enabled by default */
  cfg->caps_side_enabled  = 1U; /* Trigger sidelight ON while Caps Lock is active */
  cfg->caps_side_effect   = 1U; /* Breathing */
  cfg->tft_theme          = (uint8_t)TFT_THEME_PIXEL;

  cfg->crc32 = tk75_cfg_crc32(cfg);
}

static bool is_valid_slot(tk75_persist_cfg_t const *cand)
{
  if (cand->magic != TK75_CFG_MAGIC || cand->version != TK75_CFG_VERSION) {
    return false;
  }
  return tk75_cfg_crc32(cand) == cand->crc32;
}

uint8_t flash_eeprom_load(flash_eeprom_backend_t const *be, tk75_persist_cfg_t *out_cfg)
{
  /* Keep large config scratch buffers static so Cortex-M4 2KB stack never overflows */
  static tk75_persist_cfg_t best;
  static tk75_persist_cfg_t cand;
  bool found = false;
  uint8_t best_slot = TK75_EEPROM_SLOTS - 1U;

  memset(&best, 0, sizeof(best));
  if (be != NULL && be->read_slot != NULL) {
    for (uint8_t s = 0U; s < TK75_EEPROM_SLOTS; s++) {
      if (be->read_slot(s, &cand, sizeof(cand), be->ctx) && is_valid_slot(&cand)) {
        if (!found || (int32_t)(cand.seq - best.seq) > 0) {
          best      = cand;
          best_slot = s;
          found     = true;
        }
      }
    }
  }

  if (found) {
    *out_cfg = best;
    return best_slot;
  }

  tk75_cfg_set_defaults(out_cfg);
  return 0U;
}

bool flash_eeprom_save(flash_eeprom_backend_t const *be, tk75_persist_cfg_t *cfg, uint8_t *io_active_slot)
{
  if (be == NULL || cfg == NULL || io_active_slot == NULL) {
    return false;
  }
  if (be->erase_slot == NULL || be->write_slot == NULL) {
    return false;
  }

  uint8_t next_slot = (uint8_t)((*io_active_slot + 1U) % TK75_EEPROM_SLOTS);
  cfg->magic   = TK75_CFG_MAGIC;
  cfg->version = TK75_CFG_VERSION;
  cfg->seq++;
  cfg->crc32   = tk75_cfg_crc32(cfg);

  if (!be->erase_slot(next_slot, be->ctx)) {
    return false;
  }
  if (!be->write_slot(next_slot, cfg, sizeof(*cfg), be->ctx)) {
    return false;
  }

  *io_active_slot = next_slot;
  return true;
}

bool flash_eeprom_erase_all(flash_eeprom_backend_t const *be)
{
  if (be == NULL || be->erase_slot == NULL) {
    return false;
  }
  bool ok = true;
  for (uint8_t s = 0U; s < TK75_EEPROM_SLOTS; s++) {
    ok = be->erase_slot(s, be->ctx) && ok;
  }
  return ok;
}

#ifdef AT32F405RCT7
#include "adc_scanner_at32.h"

static uint32_t slot_flash_addr(uint8_t slot_idx)
{
  return TK75_EEPROM_BASE_ADDR + ((uint32_t)slot_idx * TK75_EEPROM_SLOT_BYTES);
}

static bool at32_read_slot(uint8_t slot_idx, void *dst, uint32_t len, void *ctx)
{
  (void)ctx;
  if (slot_idx >= TK75_EEPROM_SLOTS || len > TK75_EEPROM_SLOT_BYTES) {
    return false;
  }
  memcpy(dst, (void const *)slot_flash_addr(slot_idx), len);
  return true;
}

static bool at32_erase_slot(uint8_t slot_idx, void *ctx)
{
  (void)ctx;
  if (slot_idx >= TK75_EEPROM_SLOTS) {
    return false;
  }
  board_set_scan_timer_enabled(false);
  adc_scanner_wait_idle();
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  flash_unlock();
  uint32_t base = slot_flash_addr(slot_idx);
  flash_status_type st = FLASH_OPERATE_DONE;
  for (uint32_t off = 0U; off < TK75_EEPROM_SLOT_BYTES; off += TK75_FLASH_SECTOR_BYTES) {
    st = flash_sector_erase(base + off);
    if (st != FLASH_OPERATE_DONE) {
      break;
    }
  }
  flash_lock();
  if (primask == 0U) {
    __enable_irq();
  }
  board_set_scan_timer_enabled(true);
  return (st == FLASH_OPERATE_DONE);
}

static bool at32_write_slot(uint8_t slot_idx, void const *src, uint32_t len, void *ctx)
{
  (void)ctx;
  if (slot_idx >= TK75_EEPROM_SLOTS || len > TK75_EEPROM_SLOT_BYTES) {
    return false;
  }

  uint32_t addr = slot_flash_addr(slot_idx);
  uint8_t const *bytes = (uint8_t const *)src;
  bool ok = true;

  board_set_scan_timer_enabled(false);
  adc_scanner_wait_idle();
  uint32_t primask = __get_PRIMASK();
  __disable_irq();
  flash_unlock();
  for (uint32_t off = 0U; off < len; off += 4U) {
    uint32_t word = 0xFFFFFFFFUL;
    uint32_t rem = len - off;
    memcpy(&word, bytes + off, (rem >= 4U) ? 4U : rem);
    if (flash_word_program(addr + off, word) != FLASH_OPERATE_DONE) {
      ok = false;
      break;
    }
  }
  flash_lock();
  if (primask == 0U) {
    __enable_irq();
  }
  board_set_scan_timer_enabled(true);
  return ok;
}

static flash_eeprom_backend_t const g_at32_backend = {
  .read_slot  = at32_read_slot,
  .erase_slot = at32_erase_slot,
  .write_slot = at32_write_slot,
  .ctx        = NULL,
};

flash_eeprom_backend_t const *flash_eeprom_at32_backend(void)
{
  return &g_at32_backend;
}
#endif
