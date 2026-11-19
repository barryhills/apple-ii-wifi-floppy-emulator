cmake_minimum_required(VERSION 3.19)   # string(JSON ...) needs 3.19

set(PICO_BOARD pico2_w)
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{HOME}/pico/pico-sdk)
endif()

include(pico_sdk_import.cmake)

project(a2drive C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# ---------------------------------------------------------------------------
# Build-time defaults from config.json (gitignored; copy config.example.json).
# The Pico uses these unless a valid config is stored in flash (later).
# ---------------------------------------------------------------------------
set(A2DRIVE_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/config.json CACHE FILEPATH "WiFi/hostname defaults (JSON)")
set(CFG_JSON ${A2DRIVE_CONFIG})
if(NOT EXISTS ${CFG_JSON})
    message(FATAL_ERROR
        "firmware/config.json not found.\n"
        "Copy config.example.json to config.json and fill in your WiFi details.")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${CFG_JSON})
file(READ ${CFG_JSON} _cfg)

function(cfg_get out key)
    string(JSON _v ERROR_VARIABLE _e GET "${_cfg}" ${key})
    if(_e)
        message(FATAL_ERROR "config.json: missing or bad \"${key}\": ${_e}")
    endif()
    set(${out} "${_v}" PARENT_SCOPE)
endfunction()

cfg_get(CFG_SSID     ssid)
cfg_get(CFG_PASSWORD password)
cfg_get(CFG_HOSTNAME hostname)

string(LENGTH "${CFG_SSID}" _n)
if(_n LESS 1 OR _n GREATER 32)
    message(FATAL_ERROR "config.json: ssid must be 1-32 characters")
endif()
string(LENGTH "${CFG_PASSWORD}" _n)
if(NOT _n EQUAL 0 AND (_n LESS 8 OR _n GREATER 63))
    message(FATAL_ERROR "config.json: WPA2 password must be 8-63 characters (empty = open network)")
endif()
if(NOT CFG_HOSTNAME MATCHES "^[a-z0-9]([a-z0-9-]*[a-z0-9])?$" OR CFG_HOSTNAME MATCHES "^.{64}")
    message(FATAL_ERROR "config.json: hostname must be 1-63 of a-z, 0-9, '-' (no leading/trailing '-')")
endif()

# Escape for a C string literal.
foreach(v CFG_SSID CFG_PASSWORD)
    string(REPLACE "\\" "\\\\" ${v} "${${v}}")
    string(REPLACE "\"" "\\\"" ${v} "${${v}}")
endforeach()
configure_file(src/config_defaults.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/config_defaults.h @ONLY)

# ---------------------------------------------------------------------------
# Embed src/web/index.html as a C array (re-run on change).
# ---------------------------------------------------------------------------
set(HTML_SRC ${CMAKE_CURRENT_SOURCE_DIR}/src/web/index.html)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${HTML_SRC})
file(READ ${HTML_SRC} _html_hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _html_arr "${_html_hex}")
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/generated/index_html.c
    "// Generated from src/web/index.html. Do not edit.\n"
    "const unsigned char index_html[] = {${_html_arr}};\n"
    "const unsigned int index_html_len = sizeof index_html;\n")

# ---------------------------------------------------------------------------
add_executable(a2drive
    src/main.c
    src/net.c
    src/http.c
    src/api.c
    src/store.c
    src/clock.c
    src/json.c
    src/diag.c
    src/config.c
    src/gcr.c
    src/woz.c
    src/crc32.c
    src/status.c
    src/oled.c
    src/player.c
    src/benchtest.c
    src/drive.c
    src/third_party/dhcpserver/dhcpserver.c
    ${CMAKE_CURRENT_BINARY_DIR}/generated/index_html.c
)
target_include_directories(a2drive PRIVATE
    src                                        # lwipopts.h
    ${CMAKE_CURRENT_BINARY_DIR}/generated      # config_defaults.h
)
# -fconserve-stack: keep inlining from merging big frames. Core 0 has a 4 KB
# stack shared with lwIP's interrupt-time callbacks (see PICO_STACK_SIZE).
target_compile_options(a2drive PRIVATE -Wall -Wextra -fconserve-stack)
# Vendored code: build as-is, without our warning set.
set_source_files_properties(src/third_party/dhcpserver/dhcpserver.c PROPERTIES COMPILE_OPTIONS "-w")
target_compile_definitions(a2drive PRIVATE
    A2DRIVE_VERSION="0.13.3-led"
    PICO_STACK_SIZE=0x1000      # lwIP callbacks run in IRQ context on this stack
    PICO_USE_STACK_GUARDS=1     # M33 MSPLIM: stack overflow faults at once (see diag.c)
    PICO_STDIO_USB_CONNECT_WAIT_TIMEOUT_MS=2000   # dev aid: catch boot messages
    # Core 1 runs the drive from RAM and never reads flash (drive.c), so flash
    # writes need not stop it: flash_safe_execute only masks core 0's IRQs.
    # tools/check_core1_ram.py checks the ELF for core-1 code in flash.
    PICO_FLASH_ASSUME_CORE1_SAFE=1
)
target_link_libraries(a2drive
    pico_stdlib
    pico_cyw43_arch_lwip_threadsafe_background
    pico_lwip_mdns
    pico_lwip_sntp
    pico_unique_id
    pico_flash
    pico_multicore
    pico_sha256
    hardware_i2c
    hardware_pio
    hardware_dma
)

pico_generate_pio_header(a2drive ${CMAKE_CURRENT_SOURCE_DIR}/src/disk2.pio)
pico_generate_pio_header(a2drive ${CMAKE_CURRENT_SOURCE_DIR}/src/sample.pio)
pico_generate_pio_header(a2drive ${CMAKE_CURRENT_SOURCE_DIR}/src/wrcap.pio)
pico_generate_pio_header(a2drive ${CMAKE_CURRENT_SOURCE_DIR}/src/wrgen.pio)

pico_enable_stdio_usb(a2drive 1)
pico_enable_stdio_uart(a2drive 0)
pico_add_extra_outputs(a2drive)
