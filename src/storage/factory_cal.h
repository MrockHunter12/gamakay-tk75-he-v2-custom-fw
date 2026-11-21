#ifndef FACTORY_CAL_H
#define FACTORY_CAL_H

#include "hall_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACTORY_CAL_PAGE_BYTES 2048U

typedef struct {
  uint16_t rest_adc[HALL_SLOT_COUNT];
  uint16_t span_adc[HALL_SLOT_COUNT];
  uint8_t  switch_flags[HALL_SLOT_COUNT];
  bool     rest_valid;
  bool     span_valid;
  bool     flags_valid;
} factory_cal_data_t;

/** Parse stock AT32 flash calibration blobs (testing-friendly pure parser). */
void factory_cal_parse(uint8_t const *rest_page,
                       uint8_t const *span_page,
                       uint8_t const *flags_page,
                       factory_cal_data_t *out);

/** Load factory calibration directly from AT32 flash sectors 0x08032000..0x08033FFF. */
void factory_cal_load_flash(factory_cal_data_t *out);

#ifdef __cplusplus
}
#endif

#endif /* FACTORY_CAL_H */
