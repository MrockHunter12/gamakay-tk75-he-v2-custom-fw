/* Linker script for AT32F405RCT7 running behind GamaKay stock DFU bootloader.
 *
 * Flash layout (256 KB internal flash: 0x08000000 - 0x0803FFFF):
 *   0x08000000 - 0x08004FFF : Stock bootloader (20 KB, untouched)
 *   0x08005000 - 0x080051FF : Boot header 512 B ("AT32F405 8KMKB  ")
 *   0x08005200 - 0x08025FFF : Firmware code & rodata (131.5 KB, VTOR = 0x08005200)
 *   0x0803C000 - 0x0803FFFF : Virtual EEPROM (16 KB = 4 x 4 KB wear-leveled slots)
 *   0x08032000 - 0x08033FFF : Stock factory Hall calibration (read-only preserved)
 */

ENTRY(Reset_Handler)

_estack = 0x20019800;    /* end of 102 KB RAM */
_Min_Heap_Size  = 0x200; /* 512 B minimal heap */
_Min_Stack_Size = 0x800; /* 2 KB main stack */

MEMORY
{
  BOOT_HDR (rx) : ORIGIN = 0x08005000, LENGTH = 512
  FLASH    (rx) : ORIGIN = 0x08005200, LENGTH = 134656
  RAM      (rwx): ORIGIN = 0x20000000, LENGTH = 102K
}

SECTIONS
{
  .boot_header :
  {
    . = ALIGN(4);
    KEEP(*(.boot_header))
    . = ORIGIN(BOOT_HDR) + LENGTH(BOOT_HDR);
  } >BOOT_HDR =0xFF

  .isr_vector :
  {
    . = ALIGN(512);
    KEEP(*(.isr_vector))
    . = ALIGN(4);
  } >FLASH

  .text :
  {
    . = ALIGN(4);
    *(.text)
    *(.text*)
    *(.glue_7)
    *(.glue_7t)
    *(.eh_frame)

    KEEP(*(.init))
    KEEP(*(.fini))

    . = ALIGN(4);
    _etext = .;
  } >FLASH

  .rodata :
  {
    . = ALIGN(4);
    *(.rodata)
    *(.rodata*)
    . = ALIGN(4);
  } >FLASH

  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : {
    __exidx_start = .;
    *(.ARM.exidx*)
    __exidx_end = .;
  } >FLASH

  .preinit_array :
  {
    PROVIDE_HIDDEN(__preinit_array_start = .);
    KEEP(*(.preinit_array*))
    PROVIDE_HIDDEN(__preinit_array_end = .);
  } >FLASH

  .init_array :
  {
    PROVIDE_HIDDEN(__init_array_start = .);
    KEEP(*(SORT(.init_array.*)))
    KEEP(*(.init_array*))
    PROVIDE_HIDDEN(__init_array_end = .);
  } >FLASH

  .fini_array :
  {
    PROVIDE_HIDDEN(__fini_array_start = .);
    KEEP(*(SORT(.fini_array.*)))
    KEEP(*(.fini_array*))
    PROVIDE_HIDDEN(__fini_array_end = .);
  } >FLASH

  _sidata = LOADADDR(.data);

  .data :
  {
    . = ALIGN(4);
    _sdata = .;
    *(.data)
    *(.data*)
    . = ALIGN(4);
    _edata = .;
  } >RAM AT> FLASH

  .bss :
  {
    . = ALIGN(4);
    _sbss = .;
    __bss_start__ = _sbss;
    *(.bss)
    *(.bss*)
    *(COMMON)
    . = ALIGN(4);
    _ebss = .;
    __bss_end__ = _ebss;
  } >RAM

  ._user_heap_stack :
  {
    . = ALIGN(8);
    PROVIDE(end = .);
    PROVIDE(_end = .);
    . = . + _Min_Heap_Size;
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
  } >RAM

  /DISCARD/ :
  {
    libc.a (*)
    libm.a (*)
    libgcc.a (*)
  }

  .ARM.attributes 0 : { *(.ARM.attributes) }
}
