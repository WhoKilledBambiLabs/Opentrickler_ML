# OpenTrickler first-stage bootloader: minimal single-core binary at flash
# offset 0 that installs staged OTA images and chains into the app slot.
# See src/ota_layout.h for the flash map contract shared with the app.

add_executable(bootloader main.c)

# Only src/ota_layout.h is consumed from the application tree.
target_include_directories(bootloader PRIVATE ${CMAKE_SOURCE_DIR}/src)

target_link_libraries(bootloader
    pico_stdlib
    hardware_flash
    hardware_watchdog
    pico_bootrom
)

option(BOOTLOADER_UART_DEBUG "Enable UART debug output in the bootloader" OFF)
option(BOOTLOADER_FLASH_TRACE "Persist bootloader stage markers to the last staging sector (debug only)" OFF)
if (BOOTLOADER_FLASH_TRACE)
    target_compile_definitions(bootloader PRIVATE BOOTLOADER_FLASH_TRACE=1)
else()
    target_compile_definitions(bootloader PRIVATE BOOTLOADER_FLASH_TRACE=0)
endif()
if (BOOTLOADER_UART_DEBUG)
    pico_enable_stdio_uart(bootloader 1)
    target_compile_definitions(bootloader PRIVATE BOOTLOADER_UART_DEBUG=1)
else()
    pico_enable_stdio_uart(bootloader 0)
    target_compile_definitions(bootloader PRIVATE BOOTLOADER_UART_DEBUG=0)
endif()
pico_enable_stdio_usb(bootloader 0)

# Same script as the SDK default except FLASH is the 32 KB bootloader
# region; the link fails if the bootloader outgrows its budget.
pico_set_linker_script(bootloader ${CMAKE_SOURCE_DIR}/linker/memmap_bootloader.ld)

# The bootloader must use the plain serial 03h boot2, NOT the default
# W25Q080 one: the default leaves the QSPI flash chip in continuous-read
# mode, and the application's own boot2 (executed after rom_chain_image)
# hangs when it sends status-register commands to a chip in that state.
# With 03h reads the chip stays in standard command mode, so the chained
# app's boot2 starts from the same state as a cold ROM boot. Bootloader
# XIP speed is irrelevant (26 KB of code, runs for milliseconds).
pico_define_boot_stage2(bootloader_boot2_generic_03h ${PICO_BOOT_STAGE2_DIR}/boot2_generic_03h.S)
pico_set_boot_stage2(bootloader bootloader_boot2_generic_03h)

pico_add_extra_outputs(bootloader)

# Diagnostic target (not built by default): minimal app linked for the app
# slot that immediately reboots into BOOTSEL. A working bootloader chain is
# then visible as the RPI-RP2 drive with no display or network attached.
add_executable(chain_test EXCLUDE_FROM_ALL chain_test.c)
target_link_libraries(chain_test pico_stdlib pico_bootrom hardware_flash)
pico_set_linker_script(chain_test ${CMAKE_SOURCE_DIR}/linker/memmap_app.ld)
pico_add_extra_outputs(chain_test)
