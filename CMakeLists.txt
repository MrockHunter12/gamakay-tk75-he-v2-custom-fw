cmake_minimum_required(VERSION 3.20)
project(tk75_he_v2_via C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(AT32_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/at32f402_405_lib")
set(TINYUSB_DIR  "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tinyusb")
set(LINKER_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/linker/at32f405_tk75_dfu.ld")

set(TARGET_NAME tk75_he_v2_via)

set(AT32_DRIVER_SRCS
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_adc.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_crm.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_debug.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_dma.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_exint.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_flash.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_gpio.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_misc.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_pwc.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_scfg.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_spi.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_tmr.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_usart.c"
  "${AT32_LIB_DIR}/libraries/drivers/src/at32f402_405_wdt.c"
)

set(TINYUSB_SRCS
  "${TINYUSB_DIR}/src/tusb.c"
  "${TINYUSB_DIR}/src/common/tusb_fifo.c"
  "${TINYUSB_DIR}/src/device/usbd.c"
  "${TINYUSB_DIR}/src/class/hid/hid_device.c"
  "${TINYUSB_DIR}/src/portable/synopsys/dwc2/dcd_dwc2.c"
  "${TINYUSB_DIR}/src/portable/synopsys/dwc2/dwc2_common.c"
)

add_library(at32_sdk STATIC ${AT32_DRIVER_SRCS})
target_include_directories(at32_sdk PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bsp"
  "${AT32_LIB_DIR}/libraries/cmsis/cm4/core_support"
  "${AT32_LIB_DIR}/libraries/cmsis/cm4/device_support"
  "${AT32_LIB_DIR}/libraries/drivers/inc"
)
target_compile_definitions(at32_sdk PUBLIC
  AT32F405RCT7
  USE_STDPERIPH_DRIVER
  HEXT_VALUE=12000000U
)
target_compile_options(at32_sdk PRIVATE -O2 -Wno-unused-parameter)

add_library(tinyusb STATIC ${TINYUSB_SRCS})
target_include_directories(tinyusb PUBLIC
  "${CMAKE_CURRENT_SOURCE_DIR}/src/usb"
  "${TINYUSB_DIR}/src"
)
target_link_libraries(tinyusb PUBLIC at32_sdk)
target_compile_options(tinyusb PRIVATE -O2 -Wno-unused-parameter)

set(FW_SRCS
  "${AT32_LIB_DIR}/libraries/cmsis/cm4/device_support/startup/gcc/startup_at32f402_405.s"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bsp/system_at32f402_405.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bsp/tk75_boot_header.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bsp/board_tk75.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hall/hall_engine.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hall/hall_cal.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hall/adc_scanner_at32.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hall/encoder.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hall/mode_switches.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/factory_cal.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage/flash_eeprom.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/keymap/keymap.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/keymap/macro_player.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/keymap/macro_recorder.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/via/via_protocol.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rgb/rgb_effects.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rgb/ws2812.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tft/uart7_tft.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tft/hud_engine.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tft/pixel_hud.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rf/spi3_rf.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/usb/usb_descriptors.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/usb/usb_hid.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/main.c"
)

add_executable(${TARGET_NAME} ${FW_SRCS})
target_include_directories(${TARGET_NAME} PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/bsp"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/hall"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/storage"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/keymap"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/via"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rgb"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/tft"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/rf"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/usb"
)
target_link_libraries(${TARGET_NAME} PRIVATE tinyusb at32_sdk)
target_compile_options(${TARGET_NAME} PRIVATE -O2 -Wall -Wextra -Werror)
target_link_options(${TARGET_NAME} PRIVATE
  "-T${LINKER_SCRIPT}"
  "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.map"
  "-Wl,--print-memory-usage"
)
set_target_properties(${TARGET_NAME} PROPERTIES SUFFIX ".elf")

add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
  COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${TARGET_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin
  COMMAND ${CMAKE_OBJCOPY} -O ihex   $<TARGET_FILE:${TARGET_NAME}> ${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.hex
  COMMAND ${CMAKE_SIZE} --format=berkeley $<TARGET_FILE:${TARGET_NAME}>
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/.venv/bin/python" "${CMAKE_CURRENT_SOURCE_DIR}/tools/verify_image.py" "${CMAKE_CURRENT_BINARY_DIR}/${TARGET_NAME}.bin"
  COMMENT "Generating DFU .bin/.hex and verifying boot header + flash bounds"
)
