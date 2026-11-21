#include "usb_hid.h"
#include "usb_descriptors.h"
#include "tusb.h"
#include <string.h>

typedef struct __attribute__((packed)) {
  uint8_t modifiers;
  uint8_t keymap[16];
} nkro_report_t;

static usb_via_rx_cb_t    g_via_cb    = NULL;
static usb_led_state_cb_t g_led_cb    = NULL;
static uint8_t            g_host_leds = 0U;

void usb_hid_init(usb_via_rx_cb_t via_cb, usb_led_state_cb_t led_cb)
{
  g_via_cb = via_cb;
  g_led_cb = led_cb;
}

uint8_t usb_hid_get_host_leds(void)
{
  return g_host_leds;
}

bool usb_hid_send_boot_kbd(uint8_t modifiers, uint8_t const keys[6])
{
  if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_HID_KBD)) {
    return false;
  }
  return tud_hid_n_keyboard_report(ITF_NUM_HID_KBD, 0U, modifiers, keys);
}

bool usb_hid_send_nkro(uint8_t modifiers, uint8_t const bitmap[16])
{
  if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_HID_SHARED)) {
    return false;
  }
  nkro_report_t rep;
  rep.modifiers = modifiers;
  memcpy(rep.keymap, bitmap, sizeof(rep.keymap));
  return tud_hid_n_report(ITF_NUM_HID_SHARED, REPORT_ID_NKRO, &rep, sizeof(rep));
}

bool usb_hid_send_consumer(uint16_t usage)
{
  if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_HID_SHARED)) {
    return false;
  }
  return tud_hid_n_report(ITF_NUM_HID_SHARED, REPORT_ID_CONSUMER, &usage, sizeof(usage));
}

bool usb_hid_send_system(uint8_t usage)
{
  if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_HID_SHARED)) {
    return false;
  }
  return tud_hid_n_report(ITF_NUM_HID_SHARED, REPORT_ID_SYSTEM, &usage, sizeof(usage));
}

bool usb_hid_send_via(uint8_t const packet[32])
{
  if (!tud_mounted() || !tud_hid_n_ready(ITF_NUM_HID_VIA)) {
    return false;
  }
  return tud_hid_n_report(ITF_NUM_HID_VIA, 0U, packet, 32U);
}

bool usb_hid_is_boot_protocol(void)
{
  return tud_hid_n_get_protocol(ITF_NUM_HID_KBD) == HID_PROTOCOL_BOOT;
}

uint16_t tud_hid_get_report_cb(uint8_t instance,
                               uint8_t report_id,
                               hid_report_type_t report_type,
                               uint8_t *buffer,
                               uint16_t reqlen)
{
  (void)instance;
  (void)report_id;
  (void)report_type;
  (void)buffer;
  (void)reqlen;
  return 0U;
}

void tud_hid_set_report_cb(uint8_t instance,
                           uint8_t report_id,
                           hid_report_type_t report_type,
                           uint8_t const *buffer,
                           uint16_t bufsize)
{
  (void)report_id;
  if (instance == ITF_NUM_HID_KBD && report_type == HID_REPORT_TYPE_OUTPUT && bufsize >= 1U) {
    g_host_leds = buffer[0];
    if (g_led_cb != NULL) {
      g_led_cb(g_host_leds);
    }
    return;
  }

  if (instance == ITF_NUM_HID_VIA && bufsize > 0U && g_via_cb != NULL) {
    g_via_cb(buffer, bufsize);
  }
}
