cmake_minimum_required(VERSION 3.21)

# ---------------------------------------------------------------------------
# B-11: board and platform. Set before the SDK is imported.
# ---------------------------------------------------------------------------
set(PICO_BOARD seeed_xiao_rp2350 CACHE STRING "Board type")
set(PICO_PLATFORM rp2350-arm-s CACHE STRING "Platform")

# B-9: the SDK is the pinned git submodule unless PICO_SDK_PATH is given.
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH ${CMAKE_CURRENT_LIST_DIR}/lib/pico-sdk CACHE PATH "Pico SDK")
endif()

# B-2/B-4: locate the toolchain and prebuilt host tools installed by the
# Raspberry Pi Pico VS Code extension, unless given explicitly.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/host_tools.cmake)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/pico_sdk_import.cmake)

project(xiao_dual_uart_mag C CXX ASM)

# B-24: the application is strict C11 (-std=c11, see APP_WARNINGS below).
# The SDK's own sources use GNU extensions (e.g. "asm"), so the target as a
# whole stays on gnu11 and -std=c11 is applied to the application sources.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

if(PICO_SDK_VERSION_STRING VERSION_LESS "2.0.0")
    message(FATAL_ERROR "Pico SDK >= 2.0.0 required (found ${PICO_SDK_VERSION_STRING})")
endif()

pico_sdk_init()

option(APP_USB_STATUS "Report status over USB CDC (diagnostic builds only)" OFF)
set(APP_MODE "echo" CACHE STRING "Reference application mode: echo or pattern")
set_property(CACHE APP_MODE PROPERTY STRINGS echo pattern)

set(APP_SOURCES
    src/main.c
    src/link.c
    src/mag.c
    src/tick.c
    src/diag.c
    src/status.c
)

add_executable(firmware ${APP_SOURCES})

pico_generate_pio_header(firmware ${CMAKE_CURRENT_LIST_DIR}/src/i2c_bus_clear.pio)

target_include_directories(firmware PRIVATE ${CMAKE_CURRENT_LIST_DIR}/src)

target_link_libraries(firmware PRIVATE
    pico_stdlib
    hardware_dma
    hardware_i2c
    hardware_irq
    hardware_pio
    hardware_sync
    hardware_timer
    hardware_uart
)

if(APP_MODE STREQUAL "pattern")
    target_compile_definitions(firmware PRIVATE APP_MODE=1)
elseif(APP_MODE STREQUAL "echo")
    target_compile_definitions(firmware PRIVATE APP_MODE=0)
else()
    message(FATAL_ERROR "APP_MODE must be 'echo' or 'pattern'")
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
    )
else()
    pico_enable_stdio_usb(firmware 0)
    target_compile_definitions(firmware PRIVATE
        APP_USB_STATUS=0
        # No alarm-pool interrupt in production; nothing here needs it.
        PICO_TIME_DEFAULT_ALARM_POOL_DISABLED=1
    )
endif()

# B-22, B-23: strict warnings and stack checks on the application sources.
# (The SDK's own sources are compiled with the SDK's flags.)
set(APP_WARNINGS
    -std=c11
    -Wall -Wextra -Werror -Wshadow -Wdouble-promotion -Wconversion
    -fstack-usage -Wstack-usage=1024
)
set_source_files_properties(${APP_SOURCES} PROPERTIES COMPILE_OPTIONS "${APP_WARNINGS}")

# B-21: Release keeps debug information.
target_compile_options(firmware PRIVATE $<$<CONFIG:Release>:-g>)

pico_add_extra_outputs(firmware)
