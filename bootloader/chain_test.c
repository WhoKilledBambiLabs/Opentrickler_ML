// Diagnostic chain-test app (not part of the firmware).
//
// Linked for the app slot at 0x10008000 like the real application. Its job:
// prove the bootloader chain reaches main(), and report the machine state
// it arrived with. It writes VTOR (and MSP) into a flash trace page, then
// reboots straight into BOOTSEL - deliberately using no interrupts so it
// still completes even if VTOR points at the wrong vector table.
//
// Trace page format (flash 0x3FF000 + page*256, erased by the bootloader
// trace at entry when BOOTLOADER_FLASH_TRACE is enabled):
//   word0 magic 0xB007CAFE, word1 code, word2 value, word3 ~value
//
// Build with: cmake --build <build-dir> --target chain_test

#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picoboot_constants.h"
#include "hardware/flash.h"
#include "hardware/sync.h"

#define TRACE_SECTOR_OFFSET 0x3FF000u
#define TRACE_MAGIC         0xB007CAFEu

static void trace_mark(uint32_t page_index, uint32_t code, uint32_t value) {
    static uint8_t page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    uint32_t words[4] = {TRACE_MAGIC, code, value, ~value};

    memset(page, 0xFF, sizeof(page));
    memcpy(page, words, sizeof(words));

    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_program(TRACE_SECTOR_OFFSET + page_index * FLASH_PAGE_SIZE, page, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);
}

int main(void) {
    // Record arrival state: VTOR tells us whether the chain launch pointed
    // the CPU at this image's vector table (expect 0x10008000).
    uint32_t vtor = *(volatile uint32_t *)(PPB_BASE + 0xED08u);
    trace_mark(8, 0x41505030u, vtor);            // "APP0": reached main, value = VTOR

    uint32_t msp;
    __asm volatile ("mrs %0, msp" : "=r" (msp));
    trace_mark(9, 0x41505031u, msp);             // "APP1": value = MSP

    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}
