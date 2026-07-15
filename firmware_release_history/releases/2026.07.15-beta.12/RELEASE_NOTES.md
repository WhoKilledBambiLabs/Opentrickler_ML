# 2026.07.15-beta.12

Status: early public beta; bootloader, OTA install, failure recovery, and browser upload verified on hardware. Charge behavior is unchanged from beta 11.

## Purpose

Beta 12 replaces the riskiest part of the firmware update path. Previous betas applied OTA updates by letting the running application erase and overwrite itself; a power loss during that copy left the controller unbootable until USB recovery. Beta 12 introduces a first-stage bootloader that owns installation, survives power loss at any point, bounds failed boot attempts, and always leaves a USB recovery path. It also adds firmware upload directly from the web portal, removing the need for the Python tool in normal use.

## Firmware Changes

- A 32 KB first-stage bootloader at the start of flash installs staged firmware on reboot: verify staged CRC, copy into the application slot with per-page verification, trial-boot the result.
- The copy is restartable: install state only advances after a verified copy, so a power loss mid-install resumes and completes on the next boot.
- New firmware confirms its first successful boot immediately at startup; a firmware that never confirms is retried at most three times (guarded by a 5-second watchdog) and the device then reboots into USB recovery (BOOTSEL) instead of boot-looping.
- The application is linked for the app slot at flash 0x8000. `app.uf2` is now a combined install image (bootloader + application + cleared install metadata); `app.bin` remains the only OTA upload artifact.
- The web portal Firmware Update page uploads `app.bin` directly: client-side checksum and image checks, chunked upload with progress and cancel, install tracking through the reboot, and old-to-new version reporting.
- All `/rest/ota_*` endpoints keep their request and response shapes; `boot_state` and `bootloader_present` fields are added, and images built for the pre-bootloader layout are rejected by vector sanity checks on the device, in the browser, and in the Python tool.
- `tools/ota_upload.py` gains `--wait-for-boot` (polls through the install reboot until the new firmware confirms) and refuses `--apply` against pre-bootloader devices unless `--force-legacy` is passed.
- The bootloader and application slots are never writable over OTA; OTA flash writes are range-limited to the metadata and staging regions.

## Flash Layout

| Region | Offset | Size | Contents |
| --- | --- | --- | --- |
| Bootloader | 0x000000 | 32 KB | USB-updated only |
| Application slot | 0x008000 | 2,056,192 B | `app.bin` |
| ML history | 0x1FE000 | 8 KB | unchanged from previous betas |
| Install metadata | 0x200000 | 4 KB | staged/pending/trying/confirmed record |
| Staging | 0x201000 | 2,093,056 B | uploaded firmware image |

Saved AI models, profiles, and EEPROM settings are preserved across migration and every update.

## Migration

Devices on beta 11 or earlier must be flashed **once over USB** with the beta 12 `app.uf2` (BOOTSEL: hold the button, plug in USB, copy the file). The UF2 carries the bootloader and clears stale install state.

**Never upload the beta 12 `app.bin` through a pre-bootloader device's OTA page or tool.** The old firmware would copy it to the wrong flash address and the device would not boot until USB recovery. The new tool and web page both refuse this; old fielded firmware cannot be fixed retroactively, so the USB-first rule must be followed for the migration step.

## Verification

All on the RP2350 test device during the 2026-07-15 bench session:

- Combined `app.uf2` BOOTSEL install boots on the controller board: LCD, WiFi join, and web portal all up; EEPROM settings and AI model history intact (H1).
- Full OTA cycle via `tools/ota_upload.py --apply --wait-for-boot`: upload, bootloader install, trial boot, automatic confirmation, correct version reported (H2).
- Power pulled during the bootloader copy: install resumed and completed on the next boot. A test image that hangs before confirmation was retried three times and the device then entered BOOTSEL for USB recovery (H3).
- Browser upload end-to-end from the Firmware Update page, tracked through the reboot to confirmation (H4).
- The new tool refuses `--apply` against a beta 11 device without `--force-legacy` (H5).
- Flash dumps after BOOTSEL installs compared byte-identical to the built binaries (bootloader, full application image, metadata sector).
- The REST protocol suite passes against the `tests/web_test.py` mock, including upload, simulated reboot, chunk-failure, and stuck-trial scenarios.

Bring-up evidence: `rom_chain_image` never entered the application (flash-trace markers); the manual vector jump launch and the 03h bootloader second-stage are the verified mechanisms (see issue ledger OT-OTA-002, OT-OTA-003).

## Known Network Behavior

On Mercusys/TP-Link access points, the device's wireless security setting should be **WPA2 AES PSK**, not Mixed Mode. With Mixed Mode the device can associate and obtain an IP yet remain unreachable inbound (browser timeouts, no ping response). Switching the device setting to AES PSK resolves it (OT-NET-001).

## Recovery

- BOOTSEL USB flashing recovers the device from any state; the boot ROM's USB mode cannot be corrupted by firmware.
- Three failed boot attempts of a newly installed firmware automatically place the device in BOOTSEL.
- Flashing the combined `app.uf2` restores a known-good state without touching EEPROM settings or ML history.
