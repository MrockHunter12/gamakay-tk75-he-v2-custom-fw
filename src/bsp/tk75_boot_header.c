#include <stdint.h>

typedef struct __attribute__((packed)) {
  char     magic[16];     /* Required by stock AT32F405 bootloader: "AT32F405 8KMKB  " */
  char     fw_tag[8];     /* "TK75VIA2" */
  uint32_t vtor_addr;     /* 0x08005200 */
  uint16_t fw_major;
  uint16_t fw_minor;
  uint16_t fw_patch;
  uint16_t usb_vid;
  uint16_t usb_pid;
  uint8_t  reserved[474]; /* Padded to 512 bytes with 0xFF by linker */
} tk75_boot_header_t;

_Static_assert(sizeof(tk75_boot_header_t) == 512U, "Boot header must be 512 bytes");

__attribute__((section(".boot_header"), used))
const tk75_boot_header_t tk75_boot_header = {
  .magic     = { 'A','T','3','2','F','4','0','5',' ','8','K','M','K','B',' ',' ' },
  .fw_tag    = { 'T','K','7','5','V','I','A','2' },
  .vtor_addr = 0x08005200UL,
  .fw_major  = 1U,
  .fw_minor  = 0U,
  .fw_patch  = 0U,
  .usb_vid   = 0x3151U,
  .usb_pid   = 0x502BU,
  .reserved  = { [0 ... 473] = 0xFFU },
};
