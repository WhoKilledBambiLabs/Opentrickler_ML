# 2026.07.12-beta.12

Status: supervised correction candidate.

## Purpose

The first beta11 RL17 throw exposed an early coarse-stop regression. Coarse stopped at `20.78gn`, passively settled at `28.52gn`, and left nearly `9gn` for the fine motor. The accurate `37.48gn` result therefore took `14.42s`.

## Change

- Treats the open-loop coarse deadline strictly as a safety fallback.
- Uses the characterized bulk flow without the previous 15 percent inflation.
- Places the fallback after the expected scale-controlled handoff by including measured response latency.
- Removes the separate target-percentage duration cap that pre-empted the handoff.
- Keeps the measured coarse-tail margin, powered top-up removal, passive settle, and bounded recovery from beta11.

## Expected RL17 Behavior

At a `37.50gn` target with the current approximately `10.5gn` handoff margin, coarse should stop near `27-28gn` instead of near `21gn`. The observed approximately `8gn` tail should then land near `35-36gn`, leaving a short fine finish rather than a full fine-delivery phase.

## Validation Gate

Begin with one supervised throw. Confirm that coarse stop weight moves materially later, settled coarse weight remains below target, and final error remains within tolerance before collecting a larger set.
