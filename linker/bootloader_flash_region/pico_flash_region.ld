/* Bootloader flash region: the first 32 KB of flash (offset 0), where the
   RP2350 bootrom looks for the first-stage image. Shadows the SDK's generated
   pico_flash_region.ld (whole flash) via pico_add_linker_script_override_path().
   This is the ONLY customization vs. the SDK default memmap_default.ld; every-
   thing else tracks the SDK. The 32 KB cap is the bootloader's budget in the
   OTA flash map (see src/ota_layout.h) — the link fails if it outgrows it.
   NOTE: this sets only the linker region, not PICO_FLASH_SIZE_BYTES, so the
   bootloader can still address the full flash (metadata at 0x200000, staging). */
FLASH(rx) : ORIGIN = 0x10000000, LENGTH = 32k
