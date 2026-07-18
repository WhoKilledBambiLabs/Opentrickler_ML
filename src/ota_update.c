#include "ota_update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "common.h"
#include "flash_storage.h"
#include "ota_layout.h"
#include "hardware/flash.h"
#include "hardware/structs/watchdog.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/flash.h"
#include "pico/platform.h"

#ifndef PICO_FLASH_SIZE_BYTES
#define PICO_FLASH_SIZE_BYTES (4u * 1024u * 1024u)
#endif

// Flash layout, metadata record and CRC helpers live in ota_layout.h,
// shared with bootloader/main.c and scripts/make_combined_uf2.py.

#define OTA_MAX_ERROR_LEN             96u
#define OTA_MAX_CHUNK_BYTES           (FLASH_PAGE_SIZE * 2u)
#define OTA_APPLY_DELAY_MS            750u
#define OTA_APPLY_TASK_STACK_WORDS    1024u

// Vector sanity limits for uploaded images: word 0 is the initial stack
// pointer (must be in SRAM), word 1 the reset handler (must be inside the
// app slot). Rejects images linked for the pre-bootloader layout.
#define OTA_SRAM_BASE                 0x20000000u
#define OTA_SRAM_END                  0x20082000u  // 512 KB + scratch X/Y

typedef struct {
    bool active;
    bool verified;
    uint32_t expected_size;
    uint32_t expected_crc32;
    uint32_t actual_crc32;
    uint32_t received_size;
    char last_error[OTA_MAX_ERROR_LEN];
} ota_session_t;

typedef enum {
    OTA_FLASH_OP_ERASE = 0,
    OTA_FLASH_OP_PROGRAM = 1,
} ota_flash_op_kind_t;

typedef struct {
    ota_flash_op_kind_t kind;
    uint32_t offset;
    uint32_t length;
    const uint8_t *data;
} ota_flash_op_t;

static ota_session_t g_ota_session = {0};
static volatile bool g_ota_apply_scheduled = false;
static ota_flash_op_t g_flash_op = {0};
static uint8_t g_flash_page[OTA_MAX_CHUNK_BYTES] __attribute__((aligned(4)));
static char g_ota_response[1280];

// The layout is fixed at compile time for the 4 MB Pico 2 W flash; keep a
// build-time consistency check instead of runtime probing.
_Static_assert(OTA_LAYOUT_STAGING_OFFSET + OTA_LAYOUT_STAGING_SIZE <= PICO_FLASH_SIZE_BYTES,
               "staging region must fit the flash");

static uint32_t ota_storage_capacity(void) {
    return OTA_LAYOUT_STAGING_SIZE;
}

static uint32_t ota_primary_image_limit(void) {
    return OTA_LAYOUT_MAX_IMAGE_SIZE;
}

static bool ota_supported(void) {
    return true;
}

static const ota_metadata_t *ota_metadata_flash(void) {
    return (const ota_metadata_t *)(XIP_BASE + OTA_LAYOUT_METADATA_OFFSET);
}

static const uint8_t *ota_image_flash(void) {
    return (const uint8_t *)(XIP_BASE + OTA_LAYOUT_STAGING_OFFSET);
}

static bool ota_staged_image_has_container_magic(const uint8_t *image, uint32_t image_size) {
    if (image == NULL || image_size < 4u) {
        return false;
    }

    bool looks_like_uf2 = image[0] == 0x55u && image[1] == 0x46u &&
                          image[2] == 0x32u && image[3] == 0x0Au;
    bool looks_like_elf = image[0] == 0x7Fu && image[1] == 0x45u &&
                          image[2] == 0x4Cu && image[3] == 0x46u;
    return looks_like_uf2 || looks_like_elf;
}

static bool ota_metadata_is_valid(const ota_metadata_t *meta) {
    if (meta == NULL) {
        return false;
    }
    if (meta->magic != OTA_METADATA_MAGIC || meta->version != OTA_METADATA_VERSION) {
        return false;
    }
    if (meta->struct_crc32 != ota_metadata_struct_crc(meta)) {
        // Torn write: treat the record as blank.
        return false;
    }
    if ((meta->flags & OTA_METADATA_FLAG_VERIFIED) == 0) {
        return false;
    }
    if (meta->image_offset != OTA_LAYOUT_STAGING_OFFSET ||
        meta->primary_limit != OTA_LAYOUT_MAX_IMAGE_SIZE) {
        return false;
    }
    if (meta->image_size == 0 || meta->image_size > OTA_LAYOUT_MAX_IMAGE_SIZE) {
        return false;
    }
    return true;
}

// The install itself happens in the bootloader after this reboot; the app
// only records PENDING_INSTALL metadata and restarts cleanly.
static void ota_apply_task(void *param) {
    (void)param;

    vTaskDelay(pdMS_TO_TICKS(OTA_APPLY_DELAY_MS));
    watchdog_reboot(0, 0, 0);
    vTaskDelete(NULL);
}

static void ota_set_error(const char *message) {
    if (message == NULL) {
        message = "unknown OTA error";
    }
    snprintf(g_ota_session.last_error, sizeof(g_ota_session.last_error), "%s", message);
}

static void ota_clear_error(void) {
    g_ota_session.last_error[0] = '\0';
}

static const char *param_value(int num_params, char *params[], char *values[], const char *name) {
    for (int idx = 0; idx < num_params; idx++) {
        if (strcmp(params[idx], name) == 0) {
            return values[idx];
        }
    }
    return NULL;
}

static bool parse_u32_param(int num_params, char *params[], char *values[],
                            const char *name, uint32_t *out) {
    const char *text = param_value(num_params, params, values, name);
    if (text == NULL || out == NULL) {
        return false;
    }

    char *end = NULL;
    unsigned long value = strtoul(text, &end, 0);
    if (end == text || *end != '\0' || value > 0xFFFFFFFFul) {
        return false;
    }

    *out = (uint32_t)value;
    return true;
}

static int hex_nibble(char ch) {
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + (ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + (ch - 'A');
    }
    return -1;
}

static bool decode_hex_page(const char *hex, uint8_t *out, size_t *out_len) {
    if (hex == NULL || out == NULL || out_len == NULL) {
        return false;
    }

    size_t hex_len = strlen(hex);
    if ((hex_len == 0) || ((hex_len & 1u) != 0) || (hex_len > OTA_MAX_CHUNK_BYTES * 2u)) {
        return false;
    }

    size_t decoded_len = hex_len / 2u;
    for (size_t i = 0; i < decoded_len; i++) {
        int hi = hex_nibble(hex[i * 2u]);
        int lo = hex_nibble(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    *out_len = decoded_len;
    return true;
}

static void ota_flash_callback(void *param) {
    (void)param;

    if (g_flash_op.kind == OTA_FLASH_OP_ERASE) {
        flash_range_erase(g_flash_op.offset, g_flash_op.length);
    }
    else if (g_flash_op.kind == OTA_FLASH_OP_PROGRAM) {
        flash_range_program(g_flash_op.offset, g_flash_op.data, g_flash_op.length);
    }
}

static bool ota_flash_erase(uint32_t offset, uint32_t length) {
    if ((offset % FLASH_SECTOR_SIZE) != 0 || (length % FLASH_SECTOR_SIZE) != 0 || length == 0) {
        ota_set_error("erase request was not sector aligned");
        return false;
    }

    // The bootloader and app slot are never writable over OTA.
    if (offset < OTA_LAYOUT_METADATA_OFFSET) {
        ota_set_error("erase below the OTA region rejected");
        return false;
    }

    g_flash_op.kind = OTA_FLASH_OP_ERASE;
    g_flash_op.offset = offset;
    g_flash_op.length = length;
    g_flash_op.data = NULL;

    int rc = flash_safe_execute(ota_flash_callback, NULL, 1000);
    if (rc != PICO_OK) {
        snprintf(g_ota_session.last_error, sizeof(g_ota_session.last_error),
                 "flash erase failed rc=%d", rc);
        return false;
    }

    return true;
}

static bool ota_flash_program_page(uint32_t offset, const uint8_t *page) {
    if ((offset % FLASH_PAGE_SIZE) != 0 || page == NULL) {
        ota_set_error("program request was not page aligned");
        return false;
    }

    // The bootloader and app slot are never writable over OTA.
    if (offset < OTA_LAYOUT_METADATA_OFFSET) {
        ota_set_error("program below the OTA region rejected");
        return false;
    }

    g_flash_op.kind = OTA_FLASH_OP_PROGRAM;
    g_flash_op.offset = offset;
    g_flash_op.length = FLASH_PAGE_SIZE;
    g_flash_op.data = page;

    int rc = flash_safe_execute(ota_flash_callback, NULL, 1000);
    if (rc != PICO_OK) {
        snprintf(g_ota_session.last_error, sizeof(g_ota_session.last_error),
                 "flash program failed rc=%d", rc);
        return false;
    }

    return true;
}

static bool ota_write_metadata(uint32_t size, uint32_t expected_crc, uint32_t actual_crc,
                               uint32_t state, uint32_t app_crc) {
    ota_metadata_t meta;
    memset(&meta, 0xFF, sizeof(meta));
    meta.magic = OTA_METADATA_MAGIC;
    meta.version = OTA_METADATA_VERSION;
    meta.image_size = size;
    meta.expected_crc32 = expected_crc;
    meta.actual_crc32 = actual_crc;
    meta.flags = OTA_METADATA_FLAG_VERIFIED;
    meta.image_offset = OTA_LAYOUT_STAGING_OFFSET;
    meta.primary_limit = OTA_LAYOUT_MAX_IMAGE_SIZE;
    meta.state = state;
    meta.boot_attempts = 0;
    meta.app_crc32 = app_crc;
    meta.struct_crc32 = ota_metadata_struct_crc(&meta);

    if (!ota_flash_erase(OTA_LAYOUT_METADATA_OFFSET, FLASH_SECTOR_SIZE)) {
        return false;
    }

    memset(g_flash_page, 0xFF, sizeof(g_flash_page));
    memcpy(g_flash_page, &meta, sizeof(meta));
    return ota_flash_program_page(OTA_LAYOUT_METADATA_OFFSET, g_flash_page);
}

static bool ota_invalidate_metadata(void) {
    return ota_flash_erase(OTA_LAYOUT_METADATA_OFFSET, FLASH_SECTOR_SIZE);
}

// Called first thing in main(), before the scheduler or core 1 exist. If
// the bootloader just installed this image (state TRYING), cancel the
// bootloader's watchdog and mark the boot confirmed so the next reset is a
// plain boot. Direct flash calls with interrupts off are safe here exactly
// because nothing else is running yet; flash_safe_execute's multicore/RTOS
// lockout machinery is not available (and not needed) this early.
void ota_boot_confirm(void) {
    ota_metadata_t meta;
    memcpy(&meta, ota_metadata_flash(), sizeof(meta));

    if (meta.magic != OTA_METADATA_MAGIC || meta.version != OTA_METADATA_VERSION ||
        meta.struct_crc32 != ota_metadata_struct_crc(&meta) ||
        meta.state != OTA_BOOT_STATE_TRYING) {
        return;
    }

    // Stop the 5 s trial-boot watchdog the bootloader armed before it can
    // reset us mid-confirm.
    watchdog_disable();

    meta.state = OTA_BOOT_STATE_CONFIRMED;
    meta.struct_crc32 = ota_metadata_struct_crc(&meta);

    static uint8_t confirm_page[FLASH_PAGE_SIZE] __attribute__((aligned(4)));
    memset(confirm_page, 0xFF, sizeof(confirm_page));
    memcpy(confirm_page, &meta, sizeof(meta));

    uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(OTA_LAYOUT_METADATA_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(OTA_LAYOUT_METADATA_OFFSET, confirm_page, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);
}

static void ota_send_json(struct fs_file *file, int payload_len) {
    if (payload_len < 0) {
        payload_len = 0;
    }
    if ((size_t)payload_len >= sizeof(g_ota_response)) {
        payload_len = (int)sizeof(g_ota_response) - 1;
        g_ota_response[payload_len] = '\0';
    }

    file->data = g_ota_response;
    file->len = strlen(g_ota_response);
    file->index = file->len;
    file->flags = FS_FILE_FLAGS_HEADER_INCLUDED;
}

// Install state as reported to clients. "pending" and "trying" are only
// ever visible briefly (before the apply reboot / before confirm ran).
static const char *ota_boot_state_string(const ota_metadata_t *meta, bool meta_valid) {
    if (!meta_valid) {
        return "none";
    }
    switch (meta->state) {
        case OTA_BOOT_STATE_PENDING_INSTALL:
            return "pending";
        case OTA_BOOT_STATE_TRYING:
            return "trying";
        case OTA_BOOT_STATE_CONFIRMED:
            return "confirmed";
        default:
            return "none";
    }
}

static bool ota_build_status_json(bool success, const char *message, bool include_error) {
    const ota_metadata_t *meta = ota_metadata_flash();
    bool meta_valid = ota_metadata_is_valid(meta);
    const char *last_error = include_error ? g_ota_session.last_error : "";
    if (last_error == NULL) {
        last_error = "";
    }
    if (message == NULL) {
        message = "";
    }

    uint32_t staged_size = meta_valid ? meta->image_size : 0u;
    uint32_t staged_crc = meta_valid ? meta->actual_crc32 : 0u;

    snprintf(g_ota_response,
             sizeof(g_ota_response),
             "%s"
             "{\"success\":%s,"
             "\"message\":\"%s\","
             "\"supported\":%s,"
             "\"transport\":\"rest_get_hex\","
             "\"apply_supported\":%s,"
             "\"bootloader_required\":%s,"
             "\"bootloader_present\":true,"
             "\"boot_state\":\"%s\","
             "\"active\":%s,"
             "\"verified\":%s,"
             "\"metadata_valid\":%s,"
             "\"flash_size_bytes\":%lu,"
             "\"primary_limit_bytes\":%lu,"
             "\"staging_offset\":%lu,"
             "\"image_offset\":%lu,"
             "\"capacity_bytes\":%lu,"
             "\"expected_size\":%lu,"
             "\"received_size\":%lu,"
             "\"expected_crc32\":\"%08lX\","
             "\"actual_crc32\":\"%08lX\","
             "\"staged_size\":%lu,"
             "\"staged_crc32\":\"%08lX\","
             "\"last_error\":\"%s\"}",
             http_json_header,
             boolean_to_string(success),
             message,
             boolean_to_string(ota_supported()),
             boolean_to_string(true),   // apply always supported under the bootloader
             boolean_to_string(false),  // the bootloader is installed by definition
             ota_boot_state_string(meta, meta_valid),
             boolean_to_string(g_ota_session.active),
             boolean_to_string(g_ota_session.verified),
             boolean_to_string(meta_valid),
             (unsigned long)PICO_FLASH_SIZE_BYTES,
             (unsigned long)ota_primary_image_limit(),
             (unsigned long)OTA_LAYOUT_METADATA_OFFSET,
             (unsigned long)OTA_LAYOUT_STAGING_OFFSET,
             (unsigned long)ota_storage_capacity(),
             (unsigned long)g_ota_session.expected_size,
             (unsigned long)g_ota_session.received_size,
             (unsigned long)g_ota_session.expected_crc32,
             (unsigned long)g_ota_session.actual_crc32,
             (unsigned long)staged_size,
             (unsigned long)staged_crc,
             last_error);
    return true;
}

void ota_update_init(void) {
    const ota_metadata_t *meta = ota_metadata_flash();
    bool meta_valid = ota_metadata_is_valid(meta);

    printf("OTA: bootloader layout app=0x%06lX/%lu staging=0x%06lX/%lu boot_state=%s\n",
           (unsigned long)OTA_LAYOUT_APP_OFFSET,
           (unsigned long)OTA_LAYOUT_APP_SLOT_SIZE,
           (unsigned long)OTA_LAYOUT_STAGING_OFFSET,
           (unsigned long)OTA_LAYOUT_STAGING_SIZE,
           ota_boot_state_string(meta, meta_valid));

    // Report and clear any breadcrumb the bootloader left behind.
    uint32_t breadcrumb = watchdog_hw->scratch[3];
    if ((breadcrumb & OTA_BREADCRUMB_MASK) == OTA_BREADCRUMB_BASE) {
        printf("OTA: bootloader breadcrumb 0x%08lX\n", (unsigned long)breadcrumb);
        watchdog_hw->scratch[3] = 0;
    }
}

bool http_rest_ota_status(struct fs_file *file, int num_params, char *params[], char *values[]) {
    (void)num_params;
    (void)params;
    (void)values;

    ota_build_status_json(true, "ok", true);
    ota_send_json(file, (int)strlen(g_ota_response));
    return true;
}

bool http_rest_ota_begin(struct fs_file *file, int num_params, char *params[], char *values[]) {
    uint32_t size = 0;
    uint32_t crc32 = 0;

    memset(&g_ota_session, 0, sizeof(g_ota_session));

    if (!ota_supported()) {
        ota_set_error("OTA staging is not supported on this flash layout");
        ota_build_status_json(false, "unsupported flash layout", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (!parse_u32_param(num_params, params, values, "size", &size) ||
        !parse_u32_param(num_params, params, values, "crc32", &crc32)) {
        ota_set_error("missing size or crc32");
        ota_build_status_json(false, "missing size or crc32", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (size == 0 || size > ota_storage_capacity() || size > ota_primary_image_limit()) {
        ota_set_error("image does not fit OTA slots");
        ota_build_status_json(false, "image does not fit OTA slots", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (!ota_invalidate_metadata()) {
        ota_build_status_json(false, "could not invalidate previous OTA metadata", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    g_ota_session.active = true;
    g_ota_session.expected_size = size;
    g_ota_session.expected_crc32 = crc32;
    g_ota_session.actual_crc32 = 0;
    g_ota_session.received_size = 0;
    g_ota_session.verified = false;
    ota_clear_error();

    ota_build_status_json(true, "OTA upload started", true);
    ota_send_json(file, (int)strlen(g_ota_response));
    return true;
}

bool http_rest_ota_chunk(struct fs_file *file, int num_params, char *params[], char *values[]) {
    uint32_t offset = 0;
    const char *hex = param_value(num_params, params, values, "data");
    size_t decoded_len = 0;

    if (!g_ota_session.active) {
        ota_set_error("no OTA upload is active");
        ota_build_status_json(false, "no OTA upload is active", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (!parse_u32_param(num_params, params, values, "offset", &offset) ||
        !decode_hex_page(hex, g_flash_page, &decoded_len)) {
        ota_set_error("bad chunk request");
        ota_build_status_json(false, "bad chunk request", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (offset != g_ota_session.received_size || (offset % FLASH_PAGE_SIZE) != 0) {
        ota_set_error("chunks must be sequential and page aligned");
        ota_build_status_json(false, "chunk offset rejected", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (decoded_len > OTA_MAX_CHUNK_BYTES ||
        offset + decoded_len > g_ota_session.expected_size ||
        offset + decoded_len > ota_storage_capacity()) {
        ota_set_error("chunk exceeds expected image size");
        ota_build_status_json(false, "chunk exceeds expected image size", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if ((decoded_len % FLASH_PAGE_SIZE) != 0 &&
        offset + decoded_len != g_ota_session.expected_size) {
        ota_set_error("non-final chunks must end on a flash page boundary");
        ota_build_status_json(false, "non-final chunk alignment rejected", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    size_t program_len = decoded_len;
    if (program_len % FLASH_PAGE_SIZE != 0) {
        program_len = ((program_len / FLASH_PAGE_SIZE) + 1u) * FLASH_PAGE_SIZE;
    }

    if (decoded_len < program_len) {
        if (offset + decoded_len != g_ota_session.expected_size) {
            ota_set_error("only the final chunk may be partial");
            ota_build_status_json(false, "partial non-final chunk rejected", true);
            ota_send_json(file, (int)strlen(g_ota_response));
            return true;
        }
        memset(g_flash_page + decoded_len, 0xFF, program_len - decoded_len);
    }

    for (size_t page_offset = 0; page_offset < program_len; page_offset += FLASH_PAGE_SIZE) {
        uint32_t flash_offset = OTA_LAYOUT_STAGING_OFFSET + offset + (uint32_t)page_offset;

        if (((offset + page_offset) % FLASH_SECTOR_SIZE) == 0) {
            if (!ota_flash_erase(flash_offset, FLASH_SECTOR_SIZE)) {
                ota_build_status_json(false, "sector erase failed", true);
                ota_send_json(file, (int)strlen(g_ota_response));
                return true;
            }
        }

        if (!ota_flash_program_page(flash_offset, g_flash_page + page_offset)) {
            ota_build_status_json(false, "page program failed", true);
            ota_send_json(file, (int)strlen(g_ota_response));
            return true;
        }
    }

    size_t verify_len = decoded_len;
    const uint8_t *flash_ptr = ota_image_flash() + offset;
    if (memcmp(flash_ptr, g_flash_page, verify_len) != 0) {
        ota_set_error("page verify failed");
        ota_build_status_json(false, "page verify failed", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    g_ota_session.actual_crc32 = ota_crc32_update(g_ota_session.actual_crc32, flash_ptr, verify_len);
    g_ota_session.received_size += (uint32_t)decoded_len;
    ota_clear_error();

    ota_build_status_json(true, "chunk accepted", true);
    ota_send_json(file, (int)strlen(g_ota_response));
    return true;
}

bool http_rest_ota_finalize(struct fs_file *file, int num_params, char *params[], char *values[]) {
    (void)num_params;
    (void)params;
    (void)values;

    if (!g_ota_session.active) {
        ota_set_error("no OTA upload is active");
        ota_build_status_json(false, "no OTA upload is active", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (g_ota_session.received_size != g_ota_session.expected_size) {
        ota_set_error("upload is incomplete");
        ota_build_status_json(false, "upload is incomplete", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    uint32_t flash_crc = ota_crc32(ota_image_flash(), g_ota_session.expected_size);
    g_ota_session.actual_crc32 = flash_crc;

    if (flash_crc != g_ota_session.expected_crc32) {
        ota_set_error("CRC mismatch");
        ota_build_status_json(false, "CRC mismatch", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (!ota_write_metadata(g_ota_session.expected_size,
                            g_ota_session.expected_crc32,
                            g_ota_session.actual_crc32,
                            0xFFFFFFFFu,  // no install requested yet
                            g_ota_session.actual_crc32)) {
        ota_build_status_json(false, "metadata write failed", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    g_ota_session.verified = true;
    g_ota_session.active = false;
    ota_clear_error();

    ota_build_status_json(true, "image staged and verified", true);
    ota_send_json(file, (int)strlen(g_ota_response));
    return true;
}

bool http_rest_ota_abort(struct fs_file *file, int num_params, char *params[], char *values[]) {
    (void)num_params;
    (void)params;
    (void)values;

    memset(&g_ota_session, 0, sizeof(g_ota_session));
    if (ota_supported()) {
        ota_invalidate_metadata();
    }

    ota_build_status_json(true, "OTA upload aborted", true);
    ota_send_json(file, (int)strlen(g_ota_response));
    return true;
}

bool http_rest_ota_apply(struct fs_file *file, int num_params, char *params[], char *values[]) {
    (void)num_params;
    (void)params;
    (void)values;

    const ota_metadata_t *meta = ota_metadata_flash();
    if (!ota_metadata_is_valid(meta)) {
        ota_set_error("no verified staged firmware is available");
        ota_build_status_json(false, "no verified staged firmware is available", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    if (g_ota_apply_scheduled) {
        ota_build_status_json(true, "OTA apply already scheduled", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    uint32_t image_size = meta->image_size;
    const uint8_t *image = ota_image_flash();
    if (ota_staged_image_has_container_magic(image, image_size)) {
        ota_set_error("staged file looks like UF2/ELF; upload app.bin for OTA");
        ota_build_status_json(false, "upload app.bin, not UF2 or ELF", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    // Vector table sanity: word 0 is the initial stack pointer, word 1 the
    // reset handler. An app.bin built for the pre-bootloader layout has its
    // reset vector below the app slot and must never be installed.
    uint32_t initial_sp;
    uint32_t reset_vector;
    memcpy(&initial_sp, image, sizeof(initial_sp));
    memcpy(&reset_vector, image + 4, sizeof(reset_vector));
    if (initial_sp < OTA_SRAM_BASE || initial_sp > OTA_SRAM_END ||
        reset_vector <= (XIP_BASE + OTA_LAYOUT_APP_OFFSET) ||
        reset_vector >= (XIP_BASE + OTA_LAYOUT_APP_OFFSET + OTA_LAYOUT_APP_SLOT_SIZE)) {
        ota_set_error("image is not linked for the bootloader app slot");
        ota_build_status_json(false,
                              "image is linked for the pre-bootloader layout; build current firmware",
                              true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    uint32_t staged_crc = ota_crc32(image, image_size);
    if (staged_crc != meta->expected_crc32 || staged_crc != meta->actual_crc32) {
        ota_set_error("staged firmware CRC no longer matches metadata");
        ota_build_status_json(false, "staged firmware CRC check failed", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    // Hand the install to the bootloader: it copies staging into the app
    // slot on the next boot and trial-boots the new image.
    if (!ota_write_metadata(image_size, meta->expected_crc32, staged_crc,
                            OTA_BOOT_STATE_PENDING_INSTALL, staged_crc)) {
        ota_build_status_json(false, "could not record the install request", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    g_ota_apply_scheduled = true;
    BaseType_t task_created = xTaskCreate(ota_apply_task,
                                          "OTA Apply",
                                          OTA_APPLY_TASK_STACK_WORDS,
                                          NULL,
                                          configMAX_PRIORITIES - 1,
                                          NULL);
    if (task_created != pdPASS) {
        g_ota_apply_scheduled = false;
        ota_set_error("could not create OTA apply task");
        ota_build_status_json(false, "could not create OTA apply task", true);
        ota_send_json(file, (int)strlen(g_ota_response));
        return true;
    }

    ota_clear_error();
    ota_build_status_json(true, "install scheduled; device reboots into the bootloader", true);
    ota_send_json(file, (int)strlen(g_ota_response));
    return true;
}
