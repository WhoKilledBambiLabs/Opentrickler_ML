# 2026.07.12-beta.13

Status: supervised correction candidate.

## Evidence

The beta12 RL17 session produced 14 completed throws at `38.70gn`:

- 3 good, 11 over, and no underthrows at `+/-0.0205gn`.
- Median `12.60s`; one throw below 10 seconds.
- Mean coarse stop `22.71gn`, mean settled coarse weight `34.84gn`.
- Mean observed coarse tail `12.12gn`.
- Recovery ran on every throw and its reported endpoint gained another median `0.060gn` afterward.

Beta12 remained deadline-limited because it timed coarse with the `7.889gn/s` short-burst machine calibration even though the saved production characterization measured `5.323gn/s`.

## Changes

- Uses characterized coarse flow for open-loop deadline timing.
- Uses characterized flow for predictive handoff and limits live-flow acceleration to 120 percent of that value.
- Raises the coarse-tail telemetry ceiling so the observed `10.70-13.80gn` beta12 tails are no longer discarded.
- Uses mean plus two standard deviations from unstable production tails as a safety floor; unstable evidence can widen the handoff but cannot tighten it.
- Keeps scale-controlled handoff, passive coarse settling, and powered top-up removal.
- Reserves at least `0.050gn` when recent recovery finishes are overthrow-prone.
- Extends post-pulse motor-off observation before accepting the endpoint or issuing another recovery pulse.

## Validation Gate

Begin with three supervised throws. Coarse stop should move materially above the beta12 `22.71gn` mean while settled coarse weight must remain below target. Stop immediately for a severe overthrow or unresolved underthrow.
