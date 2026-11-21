#ifndef USB_HID_H
#define USB_HID_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Callback invoked when a 32-byte VIA command arrives on ITF_NUM_HID_VIA. */
typedef void (*usb_via_rx_cb_t)(uint8_t const *packet, uint16_t len);

/** Callback invoked when host updates Caps/Num/Scroll lock LEDs via USB OUT. */
typedef void (*usb_led_state_cb_t)(uint8_t led_mask);

/** Register VIA handler callback and host keyboard LED indicator callback. */
void usb_hid_init(usb_via_rx_cb_t via_cb, usb_led_state_cb_t led_cb);

/** Returns last received host keyboard LED bitmask (bit0=Num, bit1=Caps, bit2=Scroll). */
uint8_t usb_hid_get_host_leds(void);

/** Queue 6KRO keyboard report (ITF 0); returns false if endpoint busy. */
bool usb_hid_send_boot_kbd(uint8_t modifiers, uint8_t const keys[6]);

/** Queue 17-byte NKRO report (ITF 1, Report ID 1). */
bool usb_hid_send_nkro(uint8_t modifiers, uint8_t const bitmap[16]);

/** Queue 16-bit Consumer usage (ITF 1, Report ID 3). */
bool usb_hid_send_consumer(uint16_t usage);

/** Queue 8-bit System control usage (ITF 1, Report ID 4). */
bool usb_hid_send_system(uint8_t usage);

/** Send 32-byte VIA response packet over ITF 2 IN endpoint. */
bool usb_hid_send_via(uint8_t const packet[32]);

/** Returns true when USB host selected Boot Keyboard protocol on ITF 0 (e.g. BIOS/UEFI). */
bool usb_hid_is_boot_protocol(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_HID_H */
