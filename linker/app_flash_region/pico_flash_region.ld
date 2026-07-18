/* OTA app-slot flash region. Shadows the SDK's generated pico_flash_region.ld
   (ORIGIN 0x10000000, whole flash) via pico_add_linker_script_override_path()
   for targets linked into the OTA app slot. This is the ONLY customization vs.
   the SDK default memmap_default.ld; everything else tracks the SDK.
   ORIGIN = end of the 32 KB bootloader (0x10000000 + 0x8000).
   LENGTH  = up to the ML history region at flash offset 0x1FE000
             (0x101FE000 - 0x10008000 = 0x1F6000). See src/ota_layout.h. */
FLASH(rx) : ORIGIN = 0x10008000, LENGTH = 0x1F6000
