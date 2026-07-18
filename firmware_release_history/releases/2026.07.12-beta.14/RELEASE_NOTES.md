# 2026.07.12-beta.14

Status: supervised correction candidate.

## Evidence

Beta13 produced three complete RL17 throws at `38.70gn`:

- Two good and one `+0.040gn` overthrow.
- Times of `13.38s`, `13.56s`, and `19.26s`.
- Coarse stopped between `21.04gn` and `22.42gn` despite complete tails of `12.62-13.10gn`.
- Recovery ran on all throws; the slowest used five stalls and `2567ms` of motor time.

The handoff margin already represented powder arriving from motor stop through complete settle, but the live controller added momentum again. Guarded recovery also continued pulsing while inside the reserve intended to absorb delayed powder.

## Changes

- Removes live momentum from handoffs backed by production tail evidence.
- Uses observed maximum coarse tail plus a bounded uncertainty allowance.
- Preserves characterized-flow deadline and predictive timing from beta13.
- Treats the guarded recovery reserve as a successful motor-off endpoint.
- Reduces each recovery pulse observation from `1550ms` to `1050ms` while retaining delayed-tail protection.

## Validation Gate

Begin with three supervised throws. Require coarse stop to move above the beta13 range without settled coarse weight exceeding target. Stop for any severe overthrow or unresolved underthrow.
