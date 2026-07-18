"""Build the combined BOOTSEL install UF2: bootloader + app + blank metadata.

The output UF2 contains, in order:
  1. The first-stage bootloader at flash 0x000000.
  2. The application image at the app slot offset.
  3. One 0xFF-filled sector over the OTA metadata offset, which clears any
     stale install state (including pre-bootloader legacy metadata) when a
     device is migrated.

All flash offsets are regex-parsed from src/ota_layout.h so the layout has a
single source of truth shared with the firmware.

All blocks use the UF2 "absolute" family so the RP2350 BOOTSEL bootloader
writes them at their absolute flash addresses. The ML history region at
0x1FE000 is never written by this image.
"""

import argparse
import re
import struct
import sys

UF2_MAGIC_START0 = 0x0A324655
UF2_MAGIC_START1 = 0x9E5D5157
UF2_MAGIC_END = 0x0AB16F30
UF2_FLAG_FAMILY_ID_PRESENT = 0x00002000
UF2_ABSOLUTE_FAMILY_ID = 0xE48BFF57
UF2_BLOCK_SIZE = 512
UF2_PAYLOAD_SIZE = 256

XIP_BASE = 0x10000000
FLASH_SECTOR_SIZE = 4096

LAYOUT_CONSTANTS = (
    "OTA_LAYOUT_BOOTLOADER_SIZE",
    "OTA_LAYOUT_APP_OFFSET",
    "OTA_LAYOUT_APP_SLOT_SIZE",
    "OTA_LAYOUT_METADATA_OFFSET",
)


def parse_layout_header(path):
    """Extract the flash layout constants from src/ota_layout.h.

    Args:
        path: Path to the shared layout header.

    Returns:
        Dict mapping each LAYOUT_CONSTANTS name to its integer value.

    Raises:
        SystemExit: If any expected constant is missing from the header.
    """
    with open(path, "r", encoding="utf-8") as fp:
        text = fp.read()

    values = {}
    for name in LAYOUT_CONSTANTS:
        match = re.search(rf"#define\s+{name}\s+(0x[0-9A-Fa-f]+)u?", text)
        if match is None:
            sys.exit(f"{path}: could not find #define {name}")
        values[name] = int(match.group(1), 16)
    return values


def build_payload_blocks(data, base_addr):
    """Split raw bytes into UF2 payload tuples, padding the tail with 0xFF.

    Args:
        data: Raw bytes to place in flash.
        base_addr: Absolute flash address of the first byte.

    Returns:
        List of (addr, payload) tuples with UF2_PAYLOAD_SIZE payloads.
    """
    blocks = []
    for offset in range(0, len(data), UF2_PAYLOAD_SIZE):
        chunk = data[offset:offset + UF2_PAYLOAD_SIZE]
        if len(chunk) < UF2_PAYLOAD_SIZE:
            chunk = chunk + b"\xFF" * (UF2_PAYLOAD_SIZE - len(chunk))
        blocks.append((base_addr + offset, chunk))
    return blocks


def write_uf2(path, blocks):
    """Write (addr, payload) tuples as an absolute-family UF2 file.

    Args:
        path: Output file path.
        blocks: (addr, payload) tuples; payloads are UF2_PAYLOAD_SIZE long.
    """
    total = len(blocks)
    with open(path, "wb") as fp:
        for number, (addr, payload) in enumerate(blocks):
            block = struct.pack("<8I", UF2_MAGIC_START0, UF2_MAGIC_START1,
                                UF2_FLAG_FAMILY_ID_PRESENT, addr, UF2_PAYLOAD_SIZE,
                                number, total, UF2_ABSOLUTE_FAMILY_ID)
            block += payload
            block += b"\x00" * (508 - len(block))
            block += struct.pack("<I", UF2_MAGIC_END)
            fp.write(block)


def main(bootloader_bin_path, app_bin_path, layout_header_path, out_path):
    """Build the combined bootloader + app + blank-metadata UF2.

    Args:
        bootloader_bin_path: Raw bootloader image linked at flash offset 0.
        app_bin_path: Raw application image linked at the app slot offset.
        layout_header_path: src/ota_layout.h, the layout source of truth.
        out_path: Destination UF2 (conventionally app.uf2).

    Returns:
        None.
    """
    layout = parse_layout_header(layout_header_path)

    with open(bootloader_bin_path, "rb") as fp:
        bootloader_image = fp.read()
    with open(app_bin_path, "rb") as fp:
        app_image = fp.read()

    if len(bootloader_image) == 0 or len(app_image) == 0:
        sys.exit("bootloader or app image is empty")
    if len(bootloader_image) > layout["OTA_LAYOUT_BOOTLOADER_SIZE"]:
        sys.exit(f"bootloader is {len(bootloader_image)} bytes; "
                 f"budget is {layout['OTA_LAYOUT_BOOTLOADER_SIZE']}")
    if len(app_image) > layout["OTA_LAYOUT_APP_SLOT_SIZE"]:
        sys.exit(f"app is {len(app_image)} bytes; "
                 f"slot holds {layout['OTA_LAYOUT_APP_SLOT_SIZE']}")

    blocks = build_payload_blocks(bootloader_image, XIP_BASE)
    blocks += build_payload_blocks(app_image, XIP_BASE + layout["OTA_LAYOUT_APP_OFFSET"])
    blocks += build_payload_blocks(b"\xFF" * FLASH_SECTOR_SIZE,
                                   XIP_BASE + layout["OTA_LAYOUT_METADATA_OFFSET"])
    write_uf2(out_path, blocks)

    print(f"Combined UF2: {out_path} ({len(blocks)} blocks) - "
          f"bootloader {len(bootloader_image)} B @0x000000, "
          f"app {len(app_image)} B @0x{layout['OTA_LAYOUT_APP_OFFSET']:06X}, "
          f"metadata cleared @0x{layout['OTA_LAYOUT_METADATA_OFFSET']:06X}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bootloader-bin", required=True, help="Bootloader image linked at flash offset 0")
    parser.add_argument("--app-bin", required=True, help="Application image linked at the app slot offset")
    parser.add_argument("--layout-header", required=True, help="Path to src/ota_layout.h")
    parser.add_argument("--out", required=True, help="Output combined UF2 path")
    args = parser.parse_args()

    main(args.bootloader_bin, args.app_bin, args.layout_header, args.out)
