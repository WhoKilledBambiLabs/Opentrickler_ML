// OpenTrickler first-stage bootloader (RP2350 / Pico 2 W).
//
// Lives in the first 32 KB of flash (see src/ota_layout.h) and manages
// staged firmware installs:
//
//   - PENDING_INSTALL: verify the staged image CRC, copy it into the app
//     slot (restartable after power loss: the state only advances once the
//     copy verified), then boot it as TRYING with the watchdog armed.
//   - TRYING: count boot attempts. The app confirms a successful boot by
//     rewriting the metadata first thing in main(). After BL_TRY_LIMIT
//     failed attempts: re-copy once if the app slot is corrupt, otherwise
//     reboot into BOOTSEL so the user can recover over USB.
//   - CONFIRMED / blank metadata: chain straight into the app with zero
//     flash writes (no wear on normal boots).
//
// The bootloader itself is only ever replaced over USB, never by OTA.
// Single core, no RTOS: flash operations only need interrupts disabled.

#include <string.h>

#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "boot/picoboot_constants.h"
#include "hardware/flash.h"
#include "hardware/structs/scb.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"

#include "ota_layout.h"

#define BL_TRY_LIMIT     3u
#define BL_WATCHDOG_MS   5000u

#if BOOTLOADER_UART_DEBUG
#include <stdio.h>
#define BL_LOG(...) printf(__VA_ARGS__)
#else
#define BL_LOG(...) ((void)0)
#endif

static uint8_t g_page_buf[FLASH_PAGE_SIZE] __attribute__((aligned(4)));

static void bl_breadcrumb(uint32_t code) {
    watchdog_hw->scratch[3] = code;
}

#if BOOTLOADER_FLASH_TRACE
// Debug builds only: persist stage markers into the last staging sector so
// a hung boot can be diagnosed afterwards via picotool dumps in BOOTSEL.
// Page format: word0 magic 0xB007CAFE, word1 code, word2 value, word3 ~value.
#define BL_TRACE_SECTOR_OFFSET 0x3FF000u

static void bl_trace_erase(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(BL_TRACE_SECTOR_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(irq_state);
}

static void bl_trace_mark(uint32_t page_index, uint32_t code, uint32_t value) {
    static uint8_t trace_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    uint32_t words[4] = {0xB007CAFEu, code, value, ~value};

    memset(trace_page, 0xFF, sizeof(trace_page));
    memcpy(trace_page, words, sizeof(words));

    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_program(BL_TRACE_SECTOR_OFFSET + page_index * FLASH_PAGE_SIZE,
                        trace_page, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);
}
#else
#define bl_trace_erase() ((void)0)
#define bl_trace_mark(page_index, code, value) ((void)0)
#endif

static const uint8_t *bl_flash_ptr(uint32_t offset) {
    return (const uint8_t *)(XIP_BASE + offset);
}

static bool bl_meta_read(ota_metadata_t *meta) {
    memcpy(meta, bl_flash_ptr(OTA_LAYOUT_METADATA_OFFSET), sizeof(*meta));
    if (meta->magic != OTA_METADATA_MAGIC || meta->version != OTA_METADATA_VERSION) {
        return false;
    }
    // A torn write reads as an invalid record and is treated as blank.
    return meta->struct_crc32 == ota_metadata_struct_crc(meta);
}

static bool bl_meta_write(ota_metadata_t *meta) {
    meta->struct_crc32 = ota_metadata_struct_crc(meta);

    memset(g_page_buf, 0xFF, sizeof(g_page_buf));
    memcpy(g_page_buf, meta, sizeof(*meta));

    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(OTA_LAYOUT_METADATA_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(OTA_LAYOUT_METADATA_OFFSET, g_page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);

    return memcmp(bl_flash_ptr(OTA_LAYOUT_METADATA_OFFSET), g_page_buf, FLASH_PAGE_SIZE) == 0;
}

static void bl_meta_erase(void) {
    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(OTA_LAYOUT_METADATA_OFFSET, FLASH_SECTOR_SIZE);
    restore_interrupts(irq_state);
}

static bool bl_staged_ok(const ota_metadata_t *meta) {
    return ota_crc32(bl_flash_ptr(OTA_LAYOUT_STAGING_OFFSET), meta->image_size) == meta->app_crc32;
}

static bool bl_app_ok(const ota_metadata_t *meta) {
    return ota_crc32(bl_flash_ptr(OTA_LAYOUT_APP_OFFSET), meta->image_size) == meta->app_crc32;
}

// Copy the staged image into the app slot, sector by sector. Source bytes
// are read into a RAM buffer before each program call so no XIP read of
// the staging area happens while flash is busy.
static bool bl_copy_staged(const ota_metadata_t *meta) {
    uint32_t copy_size = (meta->image_size + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
    if (copy_size > OTA_LAYOUT_APP_SLOT_SIZE) {
        return false;
    }

    for (uint32_t sector = 0; sector < copy_size; sector += FLASH_SECTOR_SIZE) {
        uint32_t irq_state = save_and_disable_interrupts();
        flash_range_erase(OTA_LAYOUT_APP_OFFSET + sector, FLASH_SECTOR_SIZE);
        restore_interrupts(irq_state);

        for (uint32_t page = 0; page < FLASH_SECTOR_SIZE; page += FLASH_PAGE_SIZE) {
            uint32_t image_offset = sector + page;
            const uint8_t *source = bl_flash_ptr(OTA_LAYOUT_STAGING_OFFSET + image_offset);

            memset(g_page_buf, 0xFF, sizeof(g_page_buf));
            if (image_offset < meta->image_size) {
                uint32_t count = meta->image_size - image_offset;
                if (count > FLASH_PAGE_SIZE) {
                    count = FLASH_PAGE_SIZE;
                }
                memcpy(g_page_buf, source, count);
            }

            irq_state = save_and_disable_interrupts();
            flash_range_program(OTA_LAYOUT_APP_OFFSET + image_offset, g_page_buf, FLASH_PAGE_SIZE);
            restore_interrupts(irq_state);

            if (memcmp(bl_flash_ptr(OTA_LAYOUT_APP_OFFSET + image_offset),
                       g_page_buf, FLASH_PAGE_SIZE) != 0) {
                BL_LOG("BL: page verify failed at 0x%08lX\n",
                       (unsigned long)(OTA_LAYOUT_APP_OFFSET + image_offset));
                return false;
            }
        }
    }

    return bl_app_ok(meta);
}

// Reboot into the boot ROM USB bootloader (BOOTSEL) so the user can
// recover with a UF2. Never returns.
static void bl_bootsel(void) {
    watchdog_disable();
    rom_reboot(REBOOT2_FLAG_REBOOT_TYPE_BOOTSEL | REBOOT2_FLAG_NO_RETURN_ON_SUCCESS, 10, 0, 0);
    while (true) {
        tight_loop_contents();
    }
}

// Launch the app in the app slot with a manual vector jump. Returns false
// (without jumping) when the slot does not hold a plausible image.
//
// A manual jump is used instead of rom_chain_image: the ROM launch was
// observed to never enter the app on this hardware, and the manual path
// also guarantees VTOR points at the app's vector table so its interrupt
// handlers are used. Entry state matches what crt0 expects from a boot ROM
// launch: interrupts disabled, MSP from vector 0, MSPLIM cleared, thumb
// reset handler from vector 1.
static bool bl_chain_app(void) {
    const volatile uint32_t *vectors = (const volatile uint32_t *)(XIP_BASE + OTA_LAYOUT_APP_OFFSET);
    uint32_t initial_sp = vectors[0];
    uint32_t reset_handler = vectors[1];

    // Vector sanity: stack pointer in SRAM, thumb reset handler inside the
    // app slot. Rejects an erased or corrupt slot.
    if (initial_sp < 0x20000000u || initial_sp > 0x20082000u) {
        BL_LOG("BL: app slot stack pointer 0x%08lX invalid\n", (unsigned long)initial_sp);
        return false;
    }
    if ((reset_handler & 1u) == 0u ||
        reset_handler <= (XIP_BASE + OTA_LAYOUT_APP_OFFSET) ||
        reset_handler >= (XIP_BASE + OTA_LAYOUT_APP_OFFSET + OTA_LAYOUT_APP_SLOT_SIZE)) {
        BL_LOG("BL: app slot reset handler 0x%08lX invalid\n", (unsigned long)reset_handler);
        return false;
    }

    bl_breadcrumb(OTA_BREADCRUMB_CHAIN);
    bl_trace_mark(1, 0x43484E30u, reset_handler);  // "CHN0": jumping to app

    save_and_disable_interrupts();
    scb_hw->vtor = XIP_BASE + OTA_LAYOUT_APP_OFFSET;
    __asm volatile (
        "msr msplim, %[zero]\n"
        "msr msp, %[sp]\n"
        "bx %[pc]\n"
        :
        : [zero] "r" (0), [sp] "r" (initial_sp), [pc] "r" (reset_handler)
        :);
    __builtin_unreachable();
}

int main(void) {
#if BOOTLOADER_UART_DEBUG
    stdio_init_all();
#endif
    bl_breadcrumb(OTA_BREADCRUMB_START);
    bl_trace_erase();
    bl_trace_mark(0, 0x424C4E54u, 0);  // "BLNT": bootloader entered main

    ota_metadata_t meta;
    bool meta_valid = bl_meta_read(&meta);
    bool arm_watchdog = false;
    bool recovery_copy_done = false;

    if (meta_valid && meta.state == OTA_BOOT_STATE_PENDING_INSTALL) {
        if (!bl_staged_ok(&meta)) {
            // Staged image no longer matches its CRC: drop the install and
            // keep whatever is already in the app slot.
            BL_LOG("BL: staged image CRC failed, dropping install\n");
            bl_breadcrumb(OTA_BREADCRUMB_STAGED_BAD);
            bl_meta_erase();
            meta_valid = false;
        }
        else if (bl_copy_staged(&meta) || bl_copy_staged(&meta)) {
            BL_LOG("BL: staged image installed, trial boot\n");
            meta.state = OTA_BOOT_STATE_TRYING;
            meta.boot_attempts = 0;
            bl_meta_write(&meta);
            arm_watchdog = true;
            recovery_copy_done = true;
        }
        else {
            // Flash refused two full copy attempts; the staged image stays
            // intact for another try after USB recovery.
            bl_breadcrumb(OTA_BREADCRUMB_COPY_FAIL);
            bl_bootsel();
        }
    }
    else if (meta_valid && meta.state == OTA_BOOT_STATE_TRYING) {
        meta.boot_attempts++;
        if (meta.boot_attempts > BL_TRY_LIMIT) {
            if (!bl_app_ok(&meta) &&
                (meta.flags & OTA_METADATA_FLAG_RECOPIED) == 0 &&
                bl_staged_ok(&meta) &&
                bl_copy_staged(&meta)) {
                // App slot was corrupt; one repair attempt from staging.
                BL_LOG("BL: app slot repaired from staging\n");
                meta.flags |= OTA_METADATA_FLAG_RECOPIED;
                meta.boot_attempts = 1;
                bl_meta_write(&meta);
                arm_watchdog = true;
                recovery_copy_done = true;
            }
            else {
                // Image is intact but never confirmed a boot: give up and
                // hand the device to USB recovery.
                BL_LOG("BL: new firmware never confirmed, entering BOOTSEL\n");
                bl_breadcrumb(OTA_BREADCRUMB_BOOT_LOOP);
                bl_bootsel();
            }
        }
        else {
            bl_meta_write(&meta);
            arm_watchdog = true;
        }
    }
    // CONFIRMED, blank or unrecognized metadata: plain boot, no writes.

    if (arm_watchdog) {
        // The app cancels this in ota_boot_confirm() at the top of main().
        watchdog_enable(BL_WATCHDOG_MS, true);
    }

    bl_chain_app();

    // Nothing bootable in the app slot. Try one repair from staging if a
    // verified staged image exists and we have not just copied it.
    if (meta_valid && !recovery_copy_done && bl_staged_ok(&meta) && bl_copy_staged(&meta)) {
        meta.state = OTA_BOOT_STATE_TRYING;
        meta.boot_attempts = 1;
        meta.flags |= OTA_METADATA_FLAG_RECOPIED;
        bl_meta_write(&meta);
        watchdog_enable(BL_WATCHDOG_MS, true);
        bl_chain_app();
    }

    bl_breadcrumb(OTA_BREADCRUMB_NO_IMAGE);
    bl_bootsel();
}
