ENTRY(_patch_entry)

MEMORY {
  PATCH_FLASH (rx) : ORIGIN = 0x01055700, LENGTH = 0xA000
}

SECTIONS {
  .text 0x01055700 : {
    KEEP(*(.text.entry))
    *(.text .text.*)
    *(.rodata .rodata.*)
    . = ALIGN(4);
  } > PATCH_FLASH

  /DISCARD/ : {
    *(.ARM.exidx* .ARM.extab* .comment .note*)
  }
}
