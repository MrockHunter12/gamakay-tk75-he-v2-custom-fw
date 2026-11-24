#ifndef BOARD_TK75_H
#define BOARD_TK75_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callback invoked from 8 kHz (125 us) deterministic scan timer ISR. */
typedef void (*board_scan_cb_t)(void);

/** Initialize clocks (216 MHz PLL), SysTick, 1 us timer, power rails, and early Esc bootmagic. */
void board_init(void);

/** Initialize OTGHS1 PHY clocks and NVIC interrupt for TinyUSB rhport 1. */
void board_usb_otghs_init(void);

/** Register and start the 8 kHz (125 us) Hall scan tick timer (TMR6). */
void board_start_scan_timer(board_scan_cb_t cb);

/** Switch TMR6 Hall scan period in microseconds (125 us = 8 kHz .. 20000 us = 50 Hz). */
void board_set_scan_period_us(uint16_t period_us);

/** Switch TMR6 Hall scan rate between 8 kHz (125 us, slow=false) and 200 Hz (5000 us, slow=true) for battery sleep. */
void board_set_scan_rate_sleep(bool slow);

/** Pause or resume TMR6 Hall scan timer (e.g. during flash sector erase/program). */
void board_set_scan_timer_enabled(bool enabled);

/** Monotonic millisecond tick since boot. */
uint32_t board_millis(void);

/** Monotonic microsecond tick (wraps every 4294 s via TMR3 or scaled delta). */
uint32_t board_micros(void);

/** Busy-wait microsecond delay using TMR3 / DWT-free timer counter. */
void board_delay_us(uint32_t us);

/** Control WS2812 RGB 5V boost/rail power (PB13 active-HIGH). */
void board_set_rgb_power(bool on);

/** Control TFT smart display module power (PA12 active-HIGH). */
void board_set_tft_power(bool on);

/** Enable or soft-disconnect OTGHS1 USB D+/D- pull-up resistor. */
void board_usb_set_connect(bool connected);

/** Power on or fully gate OTGHS1 HS PHY + 48 MHz PLLU (saves ~25 mA in wireless battery mode). */
void board_usb_set_phy_power(bool on);

/** Enter low-power wait-for-interrupt state until next timer/SysTick/IRQ wakeup. */
void board_wfi(void);

/** Write bootloader DFU trigger magic (0x55AA55AA @ 0x08004800) and reset into stock bootloader. */
__attribute__((noreturn)) void board_enter_bootloader(void);

/** Fill 12-byte MCU unique ID for USB serial string generation. */
void board_get_uid_bytes(uint8_t out_uid[12]);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_TK75_H */
