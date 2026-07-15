#ifndef OTA_LAYOUT_H_
#define OTA_LAYOUT_H_

// Single source of truth for the OTA flash layout and install metadata.
//
// Shared between the application, the first-stage bootloader
// (bootloader/main.c) and the combined-UF2 build script
// (scripts/make_combined_uf2.py, which regex-parses the hex literals below).
// Keep every layout value here as a plain hex literal for that reason.
//
// Flash map (4 MB):
//   0x000000  bootloader (32 KB, USB-updated only, never written by OTA)
//   0x008000  application slot (app.bin linked at XIP 0x10008000)
//   0x1FE000  ML history (FLASH_STORAGE_ML_HISTORY_OFFSET, untouched)
//   0x200000  OTA metadata sector (ota_metadata_t, first page)
//   0x201000  staged image uploaded over REST

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_LAYOUT_BOOTLOADER_OFFSET  0x000000u
#define OTA_LAYOUT_BOOTLOADER_SIZE    0x008000u
#define OTA_LAYOUT_APP_OFFSET         0x008000u
#define OTA_LAYOUT_APP_SLOT_SIZE      0x1F6000u
#define OTA_LAYOUT_METADATA_OFFSET    0x200000u
#define OTA_LAYOUT_STAGING_OFFSET     0x201000u
#define OTA_LAYOUT_STAGING_SIZE       0x1FF000u

// Largest installable image: bounded by the app slot (staging is larger).
#define OTA_LAYOUT_MAX_IMAGE_SIZE     0x1F6000u

// The app slot must end exactly where the ML history region begins
// (0x1FE000, see flash_storage.h).
_Static_assert(OTA_LAYOUT_APP_OFFSET + OTA_LAYOUT_APP_SLOT_SIZE == 0x1FE000u,
               "app slot must end at the ML history region");
_Static_assert(OTA_LAYOUT_BOOTLOADER_OFFSET + OTA_LAYOUT_BOOTLOADER_SIZE == OTA_LAYOUT_APP_OFFSET,
               "app slot must start directly after the bootloader");
_Static_assert(OTA_LAYOUT_MAX_IMAGE_SIZE <= OTA_LAYOUT_APP_SLOT_SIZE &&
               OTA_LAYOUT_MAX_IMAGE_SIZE <= OTA_LAYOUT_STAGING_SIZE,
               "max image must fit both the app slot and staging");

#define OTA_METADATA_MAGIC            0x3155544Fu  // "OTU1" little-endian
#define OTA_METADATA_VERSION          2u

#define OTA_METADATA_FLAG_VERIFIED    (1u << 0)  // staged image CRC-verified
#define OTA_METADATA_FLAG_RECOPIED    (1u << 1)  // bootloader already re-copied once

// Install state machine (values chosen to be readable in hex dumps).
#define OTA_BOOT_STATE_PENDING_INSTALL 0x50454E44u  // "PEND"
#define OTA_BOOT_STATE_TRYING          0x54525947u  // "TRYG"
#define OTA_BOOT_STATE_CONFIRMED       0x434F4E46u  // "CONF"
// Any other value (including erased 0xFFFFFFFF) means no install activity.

// Bootloader breadcrumbs left in watchdog_hw->scratch[3] (survives soft
// resets; SDK only uses scratch[4..7]). The app logs and clears the value
// in ota_update_init().
#define OTA_BREADCRUMB_BASE           0xB0070000u
#define OTA_BREADCRUMB_MASK           0xFFFF0000u
#define OTA_BREADCRUMB_START          (OTA_BREADCRUMB_BASE | 0x01u)  // bootloader entered
#define OTA_BREADCRUMB_CHAIN          (OTA_BREADCRUMB_BASE | 0x02u)  // chaining into app
#define OTA_BREADCRUMB_STAGED_BAD     (OTA_BREADCRUMB_BASE | 0x03u)  // staged CRC failed; install dropped
#define OTA_BREADCRUMB_COPY_FAIL      (OTA_BREADCRUMB_BASE | 0x04u)  // copy to app slot failed twice
#define OTA_BREADCRUMB_BOOT_LOOP      (OTA_BREADCRUMB_BASE | 0x05u)  // new image never confirmed; gave up
#define OTA_BREADCRUMB_NO_IMAGE       (OTA_BREADCRUMB_BASE | 0x06u)  // nothing bootable in the app slot

// Lifecycle: ota_finalize writes the record with flags=VERIFIED and an
// erased state word; ota_apply rewrites it with state=PENDING_INSTALL and
// app_crc32; the bootloader copies staging into the app slot and writes
// TRYING with boot_attempts=0, incrementing on every retry; the app
// rewrites CONFIRMED first thing in main(); ota_begin erases the sector.
typedef struct {
    uint32_t magic;           // OTA_METADATA_MAGIC
    uint32_t version;         // OTA_METADATA_VERSION
    uint32_t image_size;      // staged image size in bytes
    uint32_t expected_crc32;  // CRC32 the uploader announced
    uint32_t actual_crc32;    // CRC32 measured over the staged flash
    uint32_t flags;           // OTA_METADATA_FLAG_*
    uint32_t image_offset;    // == OTA_LAYOUT_STAGING_OFFSET
    uint32_t primary_limit;   // == OTA_LAYOUT_MAX_IMAGE_SIZE
    uint32_t state;           // OTA_BOOT_STATE_* or erased
    uint32_t boot_attempts;   // TRYING boot counter maintained by bootloader
    uint32_t app_crc32;       // CRC32 of the image installed to the app slot
    uint32_t reserved[4];     // 0xFF fill for future use
    uint32_t struct_crc32;    // CRC32 over all preceding bytes (torn-write guard)
} ota_metadata_t;

_Static_assert(sizeof(ota_metadata_t) == 64, "metadata must stay one flash-page friendly record");

// CRC32 (IEEE, poly 0xEDB88320), shared by app, bootloader and matching the
// REST protocol / tools/ota_upload.py checksums.
static inline uint32_t ota_crc32_update(uint32_t crc, const uint8_t *data, size_t len) {
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = 0u - (crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static inline uint32_t ota_crc32(const uint8_t *data, size_t len) {
    return ota_crc32_update(0u, data, len);
}

// CRC over the metadata record excluding its trailing struct_crc32 field.
static inline uint32_t ota_metadata_struct_crc(const ota_metadata_t *meta) {
    return ota_crc32((const uint8_t *)meta, offsetof(ota_metadata_t, struct_crc32));
}

#ifdef __cplusplus
}
#endif

#endif  // OTA_LAYOUT_H_
