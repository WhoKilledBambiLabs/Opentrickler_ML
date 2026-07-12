# 2026.07.12-beta.10

Status: early public beta, code and replay verified; supervised powder testing required.

## Purpose

Beta 10 simplifies the adaptive controller around one objective: fast, accurate throws without depending on manual steering. It preserves characterized motor speeds and automatically adds protection only where recent profile-specific evidence shows a meaningful overthrow risk.

## Why Beta 9 Needed Another Step

The recovered RL17 device history contained 24 throws at a `38.40gn` target:

- Median completion time: `7.56s`.
- Sub-10-second results: `23/24`.
- Overthrows: `13/24`; underthrows: `0/24`.
- Fast finishes without recovery: `7/8` over target.
- Finishes using recovery: `6/16` over target.
- Coarse late landings: `10/24`.

The profile was already mechanically fast. Its main problem was finish control, not a lack of motor speed. Manual steering had also reached aggressive limits, so continuing to request more speed would have optimized the wrong part of the charge.

## Controller Changes

- Removed all manual steering controls and their REST endpoint.
- Kept profile characterization, machine calibration, runtime observations, and selected motor speeds.
- Preserved the persistent AI-history binary layout so existing profile characterization and observations survive the update.
- Added separate recent-history risk measurements for fast finishes, recovery finishes, and late coarse landings.
- Added complete fast-finish tail P90 and P95 estimates from the first fine stop through the post-finish peak.
- Excluded stops that were already above target from normal tail estimation so a bad handoff is not mislearned as ordinary powder flight.
- Allowed unstable but clearly dangerous evidence to widen a guard. Stable evidence is still required before a guard can tighten.
- Suppressed aggressive coarse-handoff correction when recent coarse landings are frequently late.
- Added a bounded motor-off tail-drain observation before starting recovery for a near-target deficit.
- Reduced pulse size and pulse duration when recent recovery finishes show elevated overthrow risk.
- Closed the former near-tolerance gap where a charge could enter recovery without an appropriately guarded feed decision.

## What Was Removed

- Faster.
- Safer.
- Fine Finish Faster.
- Bulk Closer.
- Undo steering.
- `/rest/ai_steering`.
- Persisted steering values in the public model response.

The web portal now reports the active adaptive-finish evidence instead of asking the operator to steer the model. Characterization and machine calibration remain the intended ways to establish the physical profile.

## Offline Replay

The private DataLab beta10 replay mirrors the firmware's phase classification and guard gates. It includes clean, unsafe-fast, risky-recovery, and sparse-history test windows; all four scenarios pass their expected safety behavior.

For the recovered RL17 window, the replay selects:

- Automatic fast-stop floor: approximately `0.360gn` below target.
- Additional anticipation over the beta 9 effective guard: approximately `0.269gn`.
- Tail-drain watch limit: approximately `0.391gn` below target.

These values are derived from historical observations. They are not a physical simulation guarantee and will adapt as the 24-observation profile window changes.

## Expected Behavior

- RL17 should remain capable of sub-10-second throws because characterized motor speeds are unchanged.
- A high-risk fast finish may stop feeding earlier and spend up to about one second observing residual powder before recovery.
- Throws that do not need the extra finish protection should not pay that delay.
- Initial beta 10 throws may be slightly slower than the fastest beta 9 throws while the controller avoids repeating the observed high-overthrow finish pattern.
- New observations replace old ones in the rolling profile window, allowing the protection to relax only after results become both stable and accurate.

## Build And Verification

- Pico 2 W release build succeeds.
- Independent release builds produce byte-identical `.bin` and `.uf2` files.
- Web portal checked at desktop and mobile widths with no console errors or obsolete steering controls.
- Static source audit confirms the steering endpoint and controller calls are gone.
- DataLab phase-guard self-test passes all scenarios.
- Persistent AI history revision remains unchanged.
- Original collector data remains untouched.
- OTA and device identity verification are pending for this release candidate.

## Powder-Test Protocol

1. Keep the existing RL17 profile and target at `38.40gn` for the first comparison.
2. Collect at least 12 supervised throws without changing characterization, calibration, tube, powder, or steering settings.
3. Stop after any severe overthrow and preserve the session before changing the profile.
4. Compare fast-finish over rate, recovery over rate, coarse-late rate, median time, P90 time, and final-error P95.
5. Accept the change only if overthrow severity falls materially while the median remains below `10s` and no unresolved underthrow appears.
6. Re-characterize only as a separate experiment after the existing-profile comparison is complete.

## Known Risks

- This is early beta firmware and the new tail-drain path has not yet been exercised with powder.
- Historical REST records cannot reproduce powder packing, vibration, tube state, scale transients, or every physical interaction.
- The first protected finishes may trade several tenths of a second for a lower overthrow risk.
- Zero overthrows and zero underthrows cannot be guaranteed with stochastic powder flow.
- Profiles remain specific to their powder, tube, motor, scale, and mechanical setup.
