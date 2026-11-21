#ifndef USB_DESCRIPTORS_H
#define USB_DESCRIPTORS_H

#ifdef __cplusplus
extern "C" {
#endif

#define USB_VID_GAMAKAY      0x3151U
#define USB_PID_TK75_VIA     0x502BU
#define USB_BCD_DEVICE       0x0100U

enum {
  ITF_NUM_HID_KBD = 0,
  ITF_NUM_HID_SHARED,
  ITF_NUM_HID_VIA,
  ITF_NUM_TOTAL
};

enum {
  REPORT_ID_NKRO = 1,
  REPORT_ID_MOUSE,
  REPORT_ID_CONSUMER,
  REPORT_ID_SYSTEM
};

#define EPNUM_HID_KBD_IN     0x81U
#define EPNUM_HID_SHARED_IN  0x82U
#define EPNUM_HID_VIA_OUT    0x03U
#define EPNUM_HID_VIA_IN     0x83U

#ifdef __cplusplus
}
#endif

#endif /* USB_DESCRIPTORS_H */
