cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# B-11: board and platform. Set before the SDK is imported.
# ---------------------------------------------------------------------------
set(PICO_BOARD seeed_xiao_rp2350 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# B-11a (Rev 2.13): the XIAO RP2350 has 2 MB of flash (Seeed product page);
# the SDK board file defaults to 4 MB. Set before the SDK is imported; the
# SDK then uses it for the linker's flash region and passes it to C code.
set(PICO_FLASH_SIZE_BYTES "2 * 1024 * 1024" CACHE STRING "Flash size (XIAO RP2350: 2 MB)")

# B-9: the SDK is the pinned git submodule unless PICO_SDK_PATH is given.
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk CACHE PATH "Pico SDK")
endif()

# B-2/B-4: locate the toolchain and prebuilt host tools installed by the
# Raspberry Pi Pico VS Code extension, unless given explicitly.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/host_tools.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(xiao_dual_uart_mag C CXX ASM)

# B-24 (Rev 2.5): every C source in the image, SDK included, is strict C11.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.0.0")
    message(FATAL_ERROR "Pico SDK >= 2.0.0 required (found ${PICO_SDK_VERSION_STRING})")
endif()

pico_sdk_init()

option(APP_USB_STATUS "Report status over USB CDC (diagnostic builds only)" OFF)
option(APP_TEST_PINS "Drive the logic-analyzer test GPIOs (HW-9)" OFF)
# Rev 2.14: user = the production application (app_user.c, 1 ms epochs);
# echo / pattern = the test application (app_test.c).
set(APP_MODE "user" CACHE STRING "Application: user, echo or pattern")
set_property(CACHE APP_MODE PROPERTY STRINGS user echo pattern)

set(APP_SOURCES
    src/main.c
    src/link.c
    src/mag.c
    src/tick.c
    src/diag.c
    src/prof.c
    src/status.c
    src/wirecheck.c
    src/crash.c
)
if(APP_MODE STREQUAL "user")
    list(APPEND APP_SOURCES src/app_user.c)
else()
    list(APPEND APP_SOURCES src/app_test.c)
endif()

add_executable(firmware ${APP_SOURCES})

pico_generate_pio_header(firmware ${CMAKE_CURRENT_LIST_DIR}/src/i2c_bus_clear.pio)

target_include_directories(firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_libraries(firmware PRIVATE
    pico_stdlib
    pico_bootrom
    hardware_dma
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_sync
    hardware_timer
    hardware_uart
    hardware_watchdog
)

if(APP_MODE STREQUAL "pattern")
    target_compile_definitions(firmware PRIVATE APP_MODE=1)
elseif(APP_MODE STREQUAL "echo")
    target_compile_definitions(firmware PRIVATE APP_MODE=0)
elseif(APP_MODE STREQUAL "user")
    target_compile_definitions(firmware PRIVATE APP_MODE=2)
else()
    message(FATAL_ERROR "APP_MODE must be 'user', 'echo' or 'pattern'")
endif()

if(APP_TEST_PINS)
    target_compile_definitions(firmware PRIVATE APP_TEST_PINS=1)
else()
    target_compile_definitions(firmware PRIVATE APP_TEST_PINS=0)
endif()

# B-18: all code executes from SRAM.
pico_set_binary_type(firmware copy_to_ram)

# B-19: GPIO0/1 belong to UART0.
pico_enable_stdio_uart(firmware 0)

# B-20: USB serial only in diagnostic builds.
if(APP_USB_STATUS)
    pico_enable_stdio_usb(firmware 1)
    target_compile_definitions(firmware PRIVATE
        APP_USB_STATUS=1
        # Never let a full CDC buffer stall the main loop for long.
        PICO_STDIO_USB_STDOUT_TIMEOUT_US=0
        PICO_STDIO_DEFAULT_CRLF=0
        # The SDK's default CDC TX buffer is 64 bytes at full speed; status
        # lines are up to ~200. status.c sends lines in pieces either way;
        # a larger buffer just takes fewer pieces.
        CFG_TUD_CDC_TX_BUFSIZE=256
        # Rev 2.23 (bring-up fix): the SDK runs tud_task() in this spare
        # IRQ. status.c gives it the USB controller's priority, so neither
        # can preempt the other (the SDK assumes they cannot).
        PICO_STDIO_USB_LOW_PRIORITY_IRQ=51
        # D-9: a panic records its message and resets, instead of halting.
        PICO_PANIC_FUNCTION=crash_panic
    )
else()
    pico_enable_stdio_usb(firmware 0)
    target_compile_definitions(firmware PRIVATE
        APP_USB_STATUS=0
        # No alarm-pool interrupt in production; nothing here needs it.
        PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1
    )
endif()

# B-22, B-23 (Rev 2.5): strict warnings and stack checks on every C source
# compiled into the image, the SDK's included. (Assembly and the SDK's one
# C++ file, pico_cxx_options/new_delete.cpp, are not C sources.)
target_compile_options(firmware PRIVATE
    $<$<COMPILE_LANGUAGE:C>:-Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion>
    $<$<COMPILE_LANGUAGE:C>:-fstack-usage -Wstack-usage=1024>
)

# The application has no C++; make the SDK's C++ allocation overrides an
# empty translation unit.
target_compile_definitions(firmware PRIVATE PICO_CXX_DISABLE_ALLOCATION_OVERRIDES=1)

# Per-file exceptions to B-22/B-24 (Rev 2.5). Each must be justified and
# limited to one file. Review this list whenever the SDK pin changes.
#  1. runtime_init.c uses the GNU keyword "asm" once (RP2350 DCP init).
#     Mapping it to "__asm" keeps the file valid strict C11; no flag is relaxed.
set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_runtime_init/runtime_init.c
    PROPERTIES COMPILE_DEFINITIONS "asm=__asm")
#  2. stdio_usb.c (diag builds only) has one -Wsign-conversion warning in
#     SDK 2.2.0 (line 224). It is still reported, but not fatal.
set_source_files_properties(
    ${PICO_SDK_PATH}/src/rp2_common/pico_stdio_usb/stdio_usb.c
    PROPERTIES COMPILE_OPTIONS "-Wno-error=sign-conversion")

# B-21: Release keeps debug information.
target_compile_options(firmware PRIVATE $<$<CONFIG:Release>:-g>)

# ---------------------------------------------------------------------------
# Rev 2.25: A/B firmware slots (partition_table.json, 2 MB flash).
#   0K      partition table (bootrom, 8 KB)
#   8K      slot A (partition 0), 1012 KB
#   1020K   slot B (partition 1), 1012 KB, linked to A
#   2032K   16 KB unpartitioned: the RP2350-E10 block, and room for settings
# The same firmware image runs from either slot (the bootrom maps the slot
# it boots to the start of the XIP window). A UF2 dragged onto the BOOTSEL
# drive goes into the slot that is not booting. The bootrom then boots it
# (flash update boot) and erases the old slot's first sector, so the new
# image stays selected.
# pt.uf2 is built next to firmware.uf2. Copy it once per board, before the
# first firmware.uf2 (README, "First-time setup").
# ---------------------------------------------------------------------------
# RP2350-E10: the A2 bootrom mishandles UF2 drag-and-drop when a partition
# table is present, unless the UF2 starts with an absolute-family block
# aimed at the end of flash. A3 and later ignore that block. Here it is
# aimed at the last sector of the 2 MB part, which is unpartitioned.
set(DFC_E10_BLOCK 0x101fff00)
set_target_properties(firmware PROPERTIES PICOTOOL_EXTRA_UF2_ARGS "--abs-block;${DFC_E10_BLOCK}")

pico_add_extra_outputs(firmware)

set(DFC_PT_JSON ${CMAKE_CURRENT_LIST_DIR}/partition_table.json)
set(DFC_PT_UF2 ${CMAKE_CURRENT_BINARY_DIR}/pt.uf2)
# pt.uf2 is in the absolute family, so it carries no E10 block. --abs-block
# only checks that the E10 sector is outside every partition. On an A2 chip,
# install pt.uf2 on a board that has no partition table yet (first-time
# setup); replacing an existing table there needs picotool, not drag-and-drop.
add_custom_command(
    OUTPUT ${DFC_PT_UF2}
    COMMAND picotool partition create --quiet ${DFC_PT_JSON} ${DFC_PT_UF2} --abs-block ${DFC_E10_BLOCK}
    DEPENDS ${DFC_PT_JSON}
    COMMENT "Partition table: pt.uf2"
    VERBATIM)
add_custom_target(partition_table ALL DEPENDS ${DFC_PT_UF2})

# The image must fit one slot (1012 KB, partition_table.json).
add_custom_command(TARGET firmware POST_BUILD
    COMMAND ${CMAKE_COMMAND} -DBIN=$<TARGET_FILE_DIR:firmware>/firmware.bin -DLIMIT=1036288
            -P ${CMAKE_CURRENT_LIST_DIR}/cmake/check_slot_size.cmake
    VERBATIM)
