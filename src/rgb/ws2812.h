#ifndef WS2812_H
#define WS2812_H

#include "rgb_effects.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Initialize SPI2 TX-only DMA (PA10 AF5) and PB13 RGB power rail (active-HIGH). */
void ws2812_init(void);

/** Convert 90 GRB pixels into SPI waveform bytes and start non-blocking DMA transfer. */
bool ws2812_show_async(rgb_grb_t const leds[RGB_LED_COUNT]);

/** Returns true when SPI2 DMA transfer is idle and ready for next frame. */
bool ws2812_is_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* WS2812_H */
