set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

if(NOT DEFINED ARM_TOOLCHAIN_ROOT)
  find_program(_ARM_GCC_IN_PATH arm-none-eabi-gcc)
  if(_ARM_GCC_IN_PATH)
    get_filename_component(_ARM_BIN_DIR "${_ARM_GCC_IN_PATH}" DIRECTORY)
    get_filename_component(_ARM_AUTO_ROOT "${_ARM_BIN_DIR}" DIRECTORY)
  elseif(EXISTS "$ENV{HOME}/.local/toolchains/arm-none-eabi/usr/bin/arm-none-eabi-gcc")
    set(_ARM_AUTO_ROOT "$ENV{HOME}/.local/toolchains/arm-none-eabi/usr")
  else()
    set(_ARM_AUTO_ROOT "/usr")
  endif()
  set(ARM_TOOLCHAIN_ROOT "${_ARM_AUTO_ROOT}" CACHE PATH "Path to GNU Arm Embedded toolchain prefix")
endif()

set(CMAKE_C_COMPILER "${ARM_TOOLCHAIN_ROOT}/bin/arm-none-eabi-gcc")
set(CMAKE_ASM_COMPILER "${ARM_TOOLCHAIN_ROOT}/bin/arm-none-eabi-gcc")
set(CMAKE_OBJCOPY "${ARM_TOOLCHAIN_ROOT}/bin/arm-none-eabi-objcopy")
set(CMAKE_SIZE "${ARM_TOOLCHAIN_ROOT}/bin/arm-none-eabi-size")

set(CPU_FLAGS "-mcpu=cortex-m4 -mthumb -mfpu=fpv4-sp-d16 -mfloat-abi=hard")

set(SYS_INC_STR "")
set(SYS_LIB_STR "")
if(EXISTS "${ARM_TOOLCHAIN_ROOT}/include/newlib")
  file(GLOB _ARM_GCC_VER_DIRS LIST_DIRECTORIES true "${ARM_TOOLCHAIN_ROOT}/lib/gcc/arm-none-eabi/*")
  list(GET _ARM_GCC_VER_DIRS 0 _ARM_GCC_VER_DIR)
  set(SYS_INC_FLAGS
    "-isystem ${_ARM_GCC_VER_DIR}/include"
    "-isystem ${_ARM_GCC_VER_DIR}/include-fixed"
    "-isystem ${ARM_TOOLCHAIN_ROOT}/include/newlib-nano"
    "-isystem ${ARM_TOOLCHAIN_ROOT}/include/newlib"
    "-isystem ${ARM_TOOLCHAIN_ROOT}/include"
  )
  string(JOIN " " SYS_INC_STR ${SYS_INC_FLAGS})

  set(SYS_LIB_FLAGS
    "-B${ARM_TOOLCHAIN_ROOT}/lib/arm-none-eabi/newlib/thumb/v7e-m+fp/hard"
    "-B${_ARM_GCC_VER_DIR}/thumb/v7e-m+fp/hard"
    "-L${ARM_TOOLCHAIN_ROOT}/lib/arm-none-eabi/newlib/thumb/v7e-m+fp/hard"
    "-L${_ARM_GCC_VER_DIR}/thumb/v7e-m+fp/hard"
  )
  string(JOIN " " SYS_LIB_STR ${SYS_LIB_FLAGS})
endif()

set(COMMON_FLAGS "${CPU_FLAGS} ${SYS_INC_STR} -ffunction-sections -fdata-sections -Wall -Wextra -Werror=implicit-function-declaration")

set(CMAKE_C_FLAGS_INIT "${COMMON_FLAGS}")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} ${SYS_LIB_STR} --specs=nosys.specs --specs=nano.specs -Wl,--gc-sections -Wl,--no-warn-rwx-segments")
