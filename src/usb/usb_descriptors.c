#include "usb_descriptors.h"
#include "board_tk75.h"
#include "tusb.h"
#include <string.h>

/* String descriptor table indices */
enum {
  STRID_LANGID = 0,
  STRID_MANUFACTURER,
  STRID_PRODUCT,
  STRID_SERIAL,
  STRID_ITF_KBD,
  STRID_ITF_SHARED,
  STRID_ITF_VIA
};

static uint8_t const desc_hid_kbd[] = {
  TUD_HID_REPORT_DESC_KEYBOARD()
};

/* NKRO (17 B) + Mouse + Consumer + System Control */
static uint8_t const desc_hid_shared[] = {
  /* Report ID 1: NKRO Keyboard (1B modifiers + 1B reserved + 16B bitmap [0x00..0x7F]) */
  0x05, 0x01,       /* Usage Page (Generic Desktop) */
  0x09, 0x06,       /* Usage (Keyboard) */
  0xA1, 0x01,       /* Collection (Application) */
    0x85, REPORT_ID_NKRO,
    0x05, 0x07,     /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xE0,     /*   Usage Min (LCtrl) */
    0x29, 0xE7,     /*   Usage Max (RGui) */
    0x15, 0x00,     /*   Logical Min (0) */
    0x25, 0x01,     /*   Logical Max (1) */
    0x95, 0x08,     /*   Report Count (8) */
    0x75, 0x01,     /*   Report Size (1) */
    0x81, 0x02,     /*   Input (Data, Var, Abs) */
    0x19, 0x00,     /*   Usage Min (0x00) */
    0x29, 0x7F,     /*   Usage Max (0x7F) */
    0x15, 0x00,     /*   Logical Min (0) */
    0x25, 0x01,     /*   Logical Max (1) */
    0x95, 0x80,     /*   Report Count (128 bits = 16 bytes) */
    0x75, 0x01,     /*   Report Size (1) */
    0x81, 0x02,     /*   Input (Data, Var, Abs) */
  0xC0,

  /* Report ID 2: Mouse */
  TUD_HID_REPORT_DESC_MOUSE(HID_REPORT_ID(REPORT_ID_MOUSE)),

  /* Report ID 3: Consumer Control (Media / Vol / Brightness) */
  0x05, 0x0C,       /* Usage Page (Consumer) */
  0x09, 0x01,       /* Usage (Consumer Control) */
  0xA1, 0x01,       /* Collection (Application) */
    0x85, REPORT_ID_CONSUMER,
    0x15, 0x00,     /*   Logical Min (0) */
    0x26, 0xFF, 0x03, /* Logical Max (0x03FF) */
    0x19, 0x00,     /*   Usage Min (0) */
    0x2A, 0xFF, 0x03, /* Usage Max (0x03FF) */
    0x75, 0x10,     /*   Report Size (16) */
    0x95, 0x01,     /*   Report Count (1) */
    0x81, 0x00,     /*   Input (Data, Array) */
  0xC0,

  /* Report ID 4: System Power Control */
  0x05, 0x01,       /* Usage Page (Generic Desktop) */
  0x09, 0x80,       /* Usage (System Control) */
  0xA1, 0x01,       /* Collection (Application) */
    0x85, REPORT_ID_SYSTEM,
    0x19, 0x81,     /*   Usage Min (System Power Down) */
    0x29, 0x83,     /*   Usage Max (System Wake Up) */
    0x15, 0x81,
    0x25, 0x83,
    0x75, 0x08,
    0x95, 0x01,
    0x81, 0x00,     /*   Input (Data, Array) */
  0xC0
};

/* Exact VIA v3 / v12 Raw HID Descriptor: Usage Page 0xFF60, Usage 0x0061, 32B IN/OUT */
static uint8_t const desc_hid_via[] = {
  0x06, 0x60, 0xFF, /* Usage Page (Vendor Defined 0xFF60) */
  0x09, 0x61,       /* Usage (0x61) */
  0xA1, 0x01,       /* Collection (Application) */
    0x09, 0x62,     /*   Usage (0x62) */
    0x15, 0x00,     /*   Logical Min (0) */
    0x26, 0xFF, 0x00, /* Logical Max (255) */
    0x95, 32,       /*   Report Count (32) */
    0x75, 8,        /*   Report Size (8) */
    0x81, 0x02,     /*   Input (Data, Var, Abs) */
    0x09, 0x63,     /*   Usage (0x63) */
    0x15, 0x00,     /*   Logical Min (0) */
    0x26, 0xFF, 0x00, /* Logical Max (255) */
    0x95, 32,       /*   Report Count (32) */
    0x75, 8,        /*   Report Size (8) */
    0x91, 0x02,     /*   Output (Data, Var, Abs) */
  0xC0
};

static tusb_desc_device_t const desc_device = {
  .bLength            = sizeof(tusb_desc_device_t),
  .bDescriptorType    = TUSB_DESC_DEVICE,
  .bcdUSB             = 0x0200U,
  .bDeviceClass       = 0x00U,
  .bDeviceSubClass    = 0x00U,
  .bDeviceProtocol    = 0x00U,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor           = USB_VID_GAMAKAY,
  .idProduct          = USB_PID_TK75_VIA,
  .bcdDevice          = USB_BCD_DEVICE,
  .iManufacturer      = STRID_MANUFACTURER,
  .iProduct           = STRID_PRODUCT,
  .iSerialNumber      = STRID_SERIAL,
  .bNumConfigurations = 0x01U,
};

static tusb_desc_device_qualifier_t const desc_device_qualifier = {
  .bLength            = sizeof(tusb_desc_device_qualifier_t),
  .bDescriptorType    = TUSB_DESC_DEVICE_QUALIFIER,
  .bcdUSB             = 0x0200U,
  .bDeviceClass       = 0x00U,
  .bDeviceSubClass    = 0x00U,
  .bDeviceProtocol    = 0x00U,
  .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
  .bNumConfigurations = 0x01U,
  .bReserved          = 0x00U,
};

#define CONFIG_TOTAL_LEN \
  (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_DESC_LEN + TUD_HID_INOUT_DESC_LEN)

/* High-Speed config: EP bInterval=1 -> 125 us (8 kHz) on keys; bInterval=4 -> 1 ms on VIA */
static uint8_t const desc_hs_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_KBD,    STRID_ITF_KBD,    HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_kbd),    EPNUM_HID_KBD_IN,    16, 1),
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_SHARED, STRID_ITF_SHARED, HID_ITF_PROTOCOL_NONE,     sizeof(desc_hid_shared), EPNUM_HID_SHARED_IN, 32, 1),
  TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_VIA, STRID_ITF_VIA, HID_ITF_PROTOCOL_NONE,     sizeof(desc_hid_via),    EPNUM_HID_VIA_OUT,   EPNUM_HID_VIA_IN, 32, 4),
};

/* Full-Speed fallback config: bInterval=1 -> 1 ms (1 kHz) */
static uint8_t const desc_fs_configuration[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN, TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 500),
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_KBD,    STRID_ITF_KBD,    HID_ITF_PROTOCOL_KEYBOARD, sizeof(desc_hid_kbd),    EPNUM_HID_KBD_IN,    16, 1),
  TUD_HID_DESCRIPTOR(ITF_NUM_HID_SHARED, STRID_ITF_SHARED, HID_ITF_PROTOCOL_NONE,     sizeof(desc_hid_shared), EPNUM_HID_SHARED_IN, 32, 1),
  TUD_HID_INOUT_DESCRIPTOR(ITF_NUM_HID_VIA, STRID_ITF_VIA, HID_ITF_PROTOCOL_NONE,     sizeof(desc_hid_via),    EPNUM_HID_VIA_OUT,   EPNUM_HID_VIA_IN, 32, 1),
};

static char const * const string_desc_arr[] = {
  (const char[]){ 0x09, 0x04 }, /* 0: English (US) */
  "GamaKay",
  "TK75 HE V2 VIA",
  NULL,                         /* 3: Generated from MCU UID */
  "TK75 Boot Keyboard",
  "TK75 NKRO & Media",
  "TK75 VIA Config",
};

static uint16_t desc_str_buf[32];

uint8_t const *tud_descriptor_device_cb(void)
{
  return (uint8_t const *)&desc_device;
}

uint8_t const *tud_descriptor_device_qualifier_cb(void)
{
  return (uint8_t const *)&desc_device_qualifier;
}

uint8_t const *tud_descriptor_configuration_cb(uint8_t index)
{
  (void)index;
  return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_hs_configuration : desc_fs_configuration;
}

uint8_t const *tud_descriptor_other_speed_configuration_cb(uint8_t index)
{
  (void)index;
  return (tud_speed_get() == TUSB_SPEED_HIGH) ? desc_fs_configuration : desc_hs_configuration;
}

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance)
{
  if (instance == ITF_NUM_HID_KBD) {
    return desc_hid_kbd;
  }
  if (instance == ITF_NUM_HID_SHARED) {
    return desc_hid_shared;
  }
  return desc_hid_via;
}

static size_t format_uid_hex(uint16_t *dst, size_t max_chars)
{
  static char const hex[] = "0123456789ABCDEF";
  uint8_t uid[12];
  size_t count = 0U;

  board_get_uid_bytes(uid);
  for (size_t i = 0U; i < sizeof(uid) && count + 2U <= max_chars; i++) {
    dst[count++] = (uint16_t)hex[(uid[i] >> 4) & 0x0FU];
    dst[count++] = (uint16_t)hex[uid[i] & 0x0FU];
  }
  return count;
}

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid)
{
  size_t chr_count = 0U;
  (void)langid;

  if (index == STRID_LANGID) {
    memcpy(&desc_str_buf[1], string_desc_arr[0], 2U);
    chr_count = 1U;
  } else if (index == STRID_SERIAL) {
    chr_count = format_uid_hex(&desc_str_buf[1], 31U);
  } else {
    if (index >= (sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))) {
      return NULL;
    }
    char const *str = string_desc_arr[index];
    if (str == NULL) {
      return NULL;
    }
    chr_count = strlen(str);
    if (chr_count > 31U) {
      chr_count = 31U;
    }
    for (size_t i = 0U; i < chr_count; i++) {
      desc_str_buf[1U + i] = (uint16_t)str[i];
    }
  }

  desc_str_buf[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2U * chr_count + 2U));
  return desc_str_buf;
}
