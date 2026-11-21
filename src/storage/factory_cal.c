#include "factory_cal.h"
#include <stddef.h>

#ifdef AT32F405RCT7
#include "tk75_pins.h"
#endif

static uint16_t read_u16_le(uint8_t const *buf, uint32_t idx)
{
  uint32_t off = idx * 2U;
  return (uint16_t)buf[off] | ((uint16_t)buf[off + 1U] << 8U);
}

static bool has_page_magic(uint8_t const *page, uint32_t magic_offset)
{
  return (page != NULL) && (page[magic_offset] == 0x55U) && (page[magic_offset + 1U] == 0xAAU);
}

void factory_cal_parse(uint8_t const *rest_page,
                       uint8_t const *span_page,
                       uint8_t const *flags_page,
                       factory_cal_data_t *out)
{
  if (out == NULL) {
    return;
  }

  out->rest_valid  = has_page_magic(rest_page, 0x7FEU);
  out->span_valid  = has_page_magic(span_page, 0x7FEU);
  out->flags_valid = has_page_magic(flags_page, 0x0FEU);

  for (uint8_t col = 0U; col < HALL_COLS; col++) {
    for (uint8_t row = 0U; row < HALL_ROWS; row++) {
      uint32_t dst_idx = ((uint32_t)row * HALL_COLS) + col;
      uint32_t src_idx = ((uint32_t)col * HALL_ROWS) + row; /* Stock layout is column-major */

      uint16_t rest = HALL_DEFAULT_REST_ADC;
      if (out->rest_valid) {
        uint16_t candidate = read_u16_le(rest_page, src_idx);
        if (candidate > 900U && candidate < 4050U) {
          rest = candidate;
        }
      }

      uint16_t span = HALL_DEFAULT_SPAN_ADC;
      if (out->span_valid) {
        uint16_t val = read_u16_le(span_page, src_idx);
        /* Stock span page may store full-travel bottom ADC (< rest) or direct delta counts */
        if (val >= 200U && val < rest - 150U) {
          span = (val < 1600U && rest > val + 400U) ? (uint16_t)(rest - val) : val;
        } else if (val >= 150U && val <= 2000U) {
          span = val;
        }
      }

      out->rest_adc[dst_idx]     = rest;
      out->span_adc[dst_idx]     = span;
      out->switch_flags[dst_idx] = out->flags_valid ? flags_page[src_idx] : 1U;
    }
  }
}

#ifdef AT32F405RCT7
void factory_cal_load_flash(factory_cal_data_t *out)
{
  factory_cal_parse((uint8_t const *)TK75_FACTORY_CAL_BASE_ADDR,
                    (uint8_t const *)TK75_FACTORY_CAL_SPAN_ADDR,
                    (uint8_t const *)TK75_FACTORY_CAL_FLAGS_ADDR,
                    out);
}
#endif
