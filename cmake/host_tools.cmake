# Locate the Arm toolchain and the prebuilt host tools (picotool, pioasm).
#
# B-4: on Windows the host tools must be the prebuilt binaries, located via
# picotool_DIR and pioasm_DIR, so no native host compiler is needed.
#
# Resolution order for each item:
#   1. a CMake variable given on the command line / in CMakeUserPresets.json
#   2. an environment variable of the same name
#   3. the default install location of the Raspberry Pi Pico VS Code
#      extension (%USERPROFILE%\.pico-sdk\...)
# If nothing is found on a non-Windows host, the SDK builds the tools from
# source with the host compiler (fine on Linux CI, not used on Windows).

set(PINNED_PICO_SDK_VERSION 2.2.0)
set(PINNED_TOOLCHAIN_VERSION 14_2_Rel1)
set(PINNED_PICOTOOL_VERSION 2.2.0)

if(WIN32 OR CMAKE_HOST_WIN32)
    file(TO_CMAKE_PATH "$ENV{USERPROFILE}" _user_home)
else()
    set(_user_home "$ENV{HOME}")
endif()
set(_pico_home "${_user_home}/.pico-sdk")

macro(_pick_path var candidate)
    if(NOT DEFINED ${var})
        if(DEFINED ENV{${var}})
            file(TO_CMAKE_PATH "$ENV{${var}}" _p)
            set(${var} "${_p}" CACHE PATH "${var}")
        elseif(EXISTS "${candidate}")
            set(${var} "${candidate}" CACHE PATH "${var}")
        endif()
    endif()
endmacro()

_pick_path(PICO_TOOLCHAIN_PATH "${_pico_home}/toolchain/${PINNED_TOOLCHAIN_VERSION}")
_pick_path(picotool_DIR "${_pico_home}/picotool/${PINNED_PICOTOOL_VERSION}/picotool")

# The extension ships pioasm in its "tools" bundle, which provides a CMake
# config file that sets pioasm_DIR.
if(NOT DEFINED pioasm_DIR AND NOT DEFINED ENV{pioasm_DIR})
    set(_tools "${_pico_home}/tools/${PINNED_PICO_SDK_VERSION}")
    if(EXISTS "${_tools}/pico-sdk-tools-config.cmake")
        set(pico-sdk-tools_DIR "${_tools}")
        include("${_tools}/pico-sdk-tools-config.cmake")
    elseif(EXISTS "${_tools}/pioasm")
        set(pioasm_DIR "${_tools}/pioasm" CACHE PATH "pioasm_DIR")
    endif()
endif()
_pick_path(pioasm_DIR "")

if(CMAKE_HOST_WIN32)
    foreach(_t picotool_DIR pioasm_DIR)
        if(NOT DEFINED ${_t})
            message(WARNING "${_t} is not set and the VS Code extension's copy was not found. "
                            "Set ${_t} to the prebuilt Windows tool (see README.md, Manual setup); "
                            "otherwise the SDK will try to build it with a host compiler (B-4).")
        endif()
    endforeach()
endif()
