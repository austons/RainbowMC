# == DO NOT EDIT THE FOLLOWING LINES for the Raspberry Pi Pico VS Code Extension to work ==
if(WIN32)
    set(USERHOME $ENV{USERPROFILE})
else()
    set(USERHOME $ENV{HOME})
endif()
set(sdkVersion 2.2.0)
set(toolchainVersion 14_2_Rel1)
set(picotoolVersion 2.2.0-a4)
set(picoVscode ${USERHOME}/.pico-sdk/cmake/pico-vscode.cmake)
if (EXISTS ${picoVscode})
    include(${picoVscode})
endif()
# ====================================================================================
set(PICO_BOARD pico CACHE STRING "Board type")

cmake_minimum_required(VERSION 3.13)

include(pico_sdk_import.cmake)

project(picomemcard_project C CXX ASM)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
# Use upstream tinyusb
set(ENV{PICO_TINYUSB_PATH} ${CMAKE_SOURCE_DIR}/tinyusb)
pico_sdk_init()

add_executable(PicoMemcard)

pico_set_linker_script(PicoMemcard ${CMAKE_SOURCE_DIR}/memmap.ld)
pico_generate_pio_header(PicoMemcard ${CMAKE_CURRENT_LIST_DIR}/psxSPI.pio)
pico_generate_pio_header(PicoMemcard ${CMAKE_CURRENT_LIST_DIR}/ws2812.pio)


# Example source
target_sources(PicoMemcard PUBLIC
    ${CMAKE_SOURCE_DIR}/src/led.c
    ${CMAKE_SOURCE_DIR}/src/lfs_flash_handler.c
    ${CMAKE_SOURCE_DIR}/src/main.c
    ${CMAKE_SOURCE_DIR}/src/memcard_simulator.c
    ${CMAKE_SOURCE_DIR}/src/memory_card.c
    ${CMAKE_SOURCE_DIR}/src/mtp_fs.c
    ${CMAKE_SOURCE_DIR}/src/unicode.c
    ${CMAKE_SOURCE_DIR}/src/usb_descriptors.c
)

# Example include
target_include_directories(PicoMemcard PUBLIC
    ${CMAKE_SOURCE_DIR}/inc
    ${CMAKE_SOURCE_DIR}/littlefs
)

add_library(littlefs INTERFACE)
target_sources(littlefs INTERFACE
  littlefs/lfs.c
  littlefs/lfs_util.c
)
target_include_directories(littlefs INTERFACE
  littlefs
)
target_compile_options(littlefs INTERFACE -Wno-unused-function -Wno-null-dereference)
target_link_libraries(littlefs INTERFACE hardware_flash)

pico_enable_stdio_uart(PicoMemcard 1)	# enable only UART stdio

target_link_libraries(PicoMemcard pico_stdlib pico_multicore pico_time hardware_flash littlefs hardware_pio tinyusb_device tinyusb_board)

pico_add_extra_outputs(PicoMemcard)