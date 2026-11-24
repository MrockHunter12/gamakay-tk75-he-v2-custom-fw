#ifndef ADC_SCANNER_AT32_H
#define ADC_SCANNER_AT32_H

#include "hall_types.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Configure PC0..5, PA0..7, PB0 analog inputs, PC6..9/PA8..9 MUX rows, ADC1, and DMA1_Ch6. */
void adc_scanner_init(void);

/** Trigger one full 6-row scan (intended to be called from 8 kHz TMR6 ISR). */
void adc_scanner_start_frame_isr(void);

/** Capture one blocking calibration/idle frame at boot (returns pointer to newest frame). */
uint16_t const *adc_scanner_capture_blocking(void);

/** Wait until any in-flight 6-row ADC/DMA scan finishes before pausing scan IRQs/flash. */
void adc_scanner_wait_idle(void);

/** Returns pointer to newly completed 6x15 frame if available, else NULL. */
uint16_t const *adc_scanner_poll_frame(void);

/** Returns true when a completed 6x15 frame is waiting to be consumed (non-destructive). */
bool adc_scanner_frame_pending(void);

/** Return latest EWMA-filtered raw VBAT ADC sample (row=5, col=4), or 0 until primed. */
uint16_t adc_scanner_get_vbat_raw(void);

/** Convert latest raw VBAT ADC sample (row=5, col=4) into filtered battery percentage (1..100). */
uint8_t adc_scanner_get_battery_percent(void);

#ifdef __cplusplus
}
#endif

#endif /* ADC_SCANNER_AT32_H */
