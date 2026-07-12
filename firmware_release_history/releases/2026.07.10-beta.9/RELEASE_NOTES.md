# 2026.07.10-beta.9

Status: early public beta, code complete and awaiting supervised powder testing.

## Purpose

Beta 9 refactors the adaptive controller around the actual production behavior observed by the flight collectors. The goal is to move repeatable profiles toward sub-10-second throws without trading that speed for frequent overthrows or unresolved underthrows.

## Data Reviewed

- 40 collector sessions containing 565 completed production throws.
- 717 characterization and calibration cycles.
- 527 MB of indexed source logs, left unchanged in the source folders.
- The recovered 24-throw RL17 device history from the missed collector session.
- H4350, RL17, Profile4, and limited AR2208 profile evidence across multiple firmware generations.

Historical firmware generations were not treated as one homogeneous training set. The runtime controller uses only the latest 24 observations for the selected profile and refuses to tighten a phase when that window is unstable.

## Root Causes

- The coarse selector rejected all characterization tails above `4gn`, even when a larger but repeatable tail produced a much faster and controllable landing.
- `time_cost_weight` did not affect coarse sample selection.
- Stop-to-settle tail already included powder flight and scale response, but the planner added another latency allowance.
- Machine calibration mixed production-speed tails with low-speed trim and micro samples.
- Saved machine handoff recommendations could increase but were effectively prevented from decreasing.
- Runtime bias learned negative values, but normal charge planning used only its positive side.
- Final recovery hid the fact that the first fine landing was usually short, so accurate final results did not teach the fast finish to land closer.

## Firmware Changes

- New characterizations rank coarse candidates by expected bulk time plus tail risk.
- Candidate safety limits scale with target weight and delivered mass instead of using a universal `4gn` cutoff.
- Characterization now fails closed instead of silently selecting an unsafe fallback when no candidate passes those limits.
- Existing saved motor choices remain unchanged after an update. A fresh characterization is required before the new selector can choose a different production speed.
- Machine calibration now uses the first production-speed samples for production tail, timing, uncertainty, and flow; low-speed samples update only trim or micro flow.
- Stop-to-settle tail is treated as the complete transport and latency observation, with only a bounded repeatability allowance added.
- The latest 24 observations produce per-profile mean, standard deviation, P90, P95, maximum, recovery, timing, overthrow, and underthrow statistics.
- At least six stable observations are required before production statistics may tighten a handoff.
- Stable coarse history may cap an early machine handoff, but P95-plus-variance and fine-reserve floors remain mandatory.
- Stable fine history constrains the fast stop between P90 and P95 guards. Recovery still handles residual underthrows.
- Native measured coarse trim is no longer contaminated by the high-speed bulk tail. Synthesized glide remains conservatively guarded.
- Negative runtime bias now contributes to a closer handoff within a bounded limit.
- Pre-recovery fine landing error lowers an inflated fast-tail estimate and stop-safety bias more quickly.
- Applying a new characterization or machine calibration clears stale runtime observations only for that profile.
- REST AI history includes the exact runtime statistics used by the planner.

## Offline Replay

The DataLab replay uses the same 24-observation window and stability gates as the firmware.

- RL17: stable coarse and fine tails; counterfactual median estimate `9.05s`, down from `13.00s` in the recovered window.
- Profile4: stable coarse and fine tails; counterfactual median estimate `11.10s`, down from `16.20s`.
- H4350: latest coarse and fine windows are too variable, so beta 9 correctly retains the conservative path and produces no speed estimate.

These are engineering estimates from observed phase distributions, not a physical simulation and not a guarantee. The next supervised powder session is the deciding test.

## Build Status

- Pico 2 W release build succeeds.
- Persistent AI history revision remains unchanged.
- Original collector data remains untouched.
- Offline controller replay and characterization-selector audit complete.
- OTA staging, CRC verification, apply, reboot, and device-identity verification succeeded on a Pico 2 W.
- The device reported `2026.07.10-beta.9`, source identity `3c24275-dirty`, and `Release` after reboot.
- Physical powder testing has not yet been performed for beta 9.

## Powder-Test Focus

1. Begin with the existing RL17 profile and collect at least 10 supervised throws at one target.
2. Watch `bulk_handoff_gn`, coarse stop and settle weights, fine stop and settle weights, recovery motor time, and final peak.
3. Stop testing immediately after a coarse over-target landing or a severe final overthrow.
4. Compare the first six conservative throws with later throws after the stability gate unlocks.
5. Test a fresh characterization and machine calibration separately; do not combine that first validation with an unsupervised long run.

## Known Risks

- Physical results may differ from counterfactual estimates because historical logs cannot reproduce powder packing, vibration, tube state, or every scale transient.
- A newly selected faster coarse speed must be followed by machine calibration and supervised validation.
- Recovery remains necessary because a strict guarantee of zero underthrows and zero overthrows is not physically realistic with stochastic powder flow.
- Profiles with unstable recent tails intentionally receive little or no speed improvement until their observations stabilize.
