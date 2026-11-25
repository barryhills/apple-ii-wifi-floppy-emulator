cmake_minimum_required(VERSION 3.13)

# Milestone 0: prove the toolchain, flashing, the CYW43 LED path and USB serial.
set(PICO_BOARD pico2_w)
if(NOT DEFINED PICO_SDK_PATH AND NOT DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH $ENV{HOME}/pico/pico-sdk)
endif()

include(pico_sdk_import.cmake)

project(blink C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

add_executable(blink blink.c)
target_compile_options(blink PRIVATE -Wall -Wextra)
target_link_libraries(blink pico_stdlib pico_cyw43_arch_none pico_unique_id)

pico_enable_stdio_usb(blink 1)
pico_enable_stdio_uart(blink 0)
pico_add_extra_outputs(blink)
