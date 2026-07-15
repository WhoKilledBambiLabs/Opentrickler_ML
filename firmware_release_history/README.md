# OpenTrickler ML Firmware Release History

This folder is the release ledger for the public beta firmware branch.

## Contents

- `CHANGELOG.md`: chronological firmware history.
- `ISSUE_LEDGER.md`: bugs, fixes, and verification status.
- `releases/<version>/RELEASE_NOTES.md`: detailed notes for one firmware beta.

Release binaries should be produced by local builds or GitHub Actions and attached as GitHub release assets when they are ready to share.

## Release Procedure

1. Update `RELEASE_VERSION`.
2. Build with `cmake --build build-pico2w-release --config Release`.
3. Confirm the generated version information.
4. Flash by USB or OTA in a supervised beta test. Devices on beta 11 or earlier take the new firmware only by USB (`app.uf2` carries the OTA bootloader); never send the new `app.bin` through a pre-bootloader device's OTA.
5. Verify the device-reported version after reboot; on OTA installs confirm `boot_state` reaches `confirmed` (web firmware page or `tools/ota_upload.py --apply --wait-for-boot`).
6. Update the release notes with public-safe behavior, verification, known risks, and test focus.
7. Update the issue ledger with any newly verified or newly discovered behavior.

Code-complete fixes remain `awaiting-powder-test` in the issue ledger until a physical session exercises the affected behavior.
