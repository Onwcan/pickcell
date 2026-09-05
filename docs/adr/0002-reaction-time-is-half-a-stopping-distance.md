# ADR-0002: A reaction time is half a stopping distance

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Onur Can Urhan

## Context

ADR-0001 established what this repository measures: the interval between the
safety runtime deciding and the cell stopping its command. The evidence files
report it in milliseconds — 1.07 ms worst case over shared memory, 96.1 ms over
a 100 ms HTTP poll.

Milliseconds are not what a cell is laid out in, and nobody positions a fence
from one. The question a reaction time is a step towards is *how far does the
tool travel after the hazard is detected*, because that is what decides how far
back a guard, a light curtain or a fence has to sit.

There is also something the measurement quietly assumes. `Cell::step` stops
commanding motion the instant it learns, and the reaction figure ends there. A
real arm does not stop when it is told to; it decelerates under whatever
acceleration and jerk it is capable of, and that takes longer than the link
did.

## Decision

`pickcell::stoppingDistance` reports both halves separately and their sum:

    reaction_travel = speed × reaction_time          (measured here)
    braking_distance = motionkit::StopProfile(speed)  (jerk-limited)
    total = reaction_travel + braking_distance

This is ISO 13855's minimum-distance calculation in the form this cell can
compute for itself. Keeping the two terms apart is the point: one is a property
of the integration and is the thing this repository can change, the other is a
property of the axis and is not.

`pickcell::permittedSpeed` inverts it — given the clearance a cell actually has,
how fast may it run. Solved by bisection, because the sum is strictly increasing
in speed so the root is unique; a closed form exists only where the acceleration
plateau is reached, and the test checks the two against each other there rather
than trusting either alone.

Reaction figures used in the report are **worst cases, not medians**. A guard is
positioned for the slowest reaction the system can have. For a poll that is not
a measurement at all — an event at a uniformly random moment inside a fixed
window is discovered at worst a full window later, which is why the measured
maxima in ADR-0001 match the poll intervals to within a millisecond.

## What the numbers say

At the axis limits this cell uses — 2 m/s, 8 m/s², 40 m/s³ — from
[`evidence/safety-distance.txt`](../../evidence/safety-distance.txt):

| speed | link | reaction | braking | total |
|---:|---|---:|---:|---:|
| 0.25 m/s | shared memory | 0.3 mm | 19.8 mm | **20.0 mm** |
| 0.25 m/s | HTTP poll 200 ms | 47.8 mm | 19.8 mm | **67.5 mm** |
| 2.00 m/s | shared memory | 2.2 mm | 450.0 mm | **452.2 mm** |
| 2.00 m/s | HTTP poll 200 ms | 382.0 mm | 450.0 mm | **832.0 mm** |

Inverted, with 200 mm of clearance: shared memory permits **1.16 m/s** and a
200 ms poll permits **0.63 m/s**. The link choice nearly halves the cell's
speed, which is cycle time rather than a diagnostic curiosity.

### The penalty runs the other way to the instinct

Swapping shared memory for a 200 ms poll multiplies the guard distance by
**3.4×** at 0.25 m/s and by only **1.8×** at 2.00 m/s.

The absolute cost is worse on a fast cell — 380 mm against 47 mm — and the
*relative* cost is worse on a slow one. Braking distance grows faster than
linearly with speed while reaction travel grows linearly, so a slow machine has
almost no braking distance to hide a slow link behind. "We run slowly here, so
latency does not matter" has it exactly backwards.

## Consequences

- The poll interval is floor space. Every millisecond of reaction costs the
  tool's speed in millimetres of clearance, and a cell that cannot have the
  clearance pays for it in cycle time instead.
- **The braking term is a floor this repository cannot lower.** No link, however
  fast, gets below it — 450 mm at 2 m/s. That is the honest limit on what the
  measurement in ADR-0001 can buy, and it is worth stating because the reaction
  figures on their own invite the opposite conclusion.
- The report assumes the axis is at **constant speed** when the hazard occurs,
  which is the assumption a guard calculation is normally allowed to make. An
  axis still accelerating travels further; motionkit's `StopProfile` reports how
  much further from a real state, and this cell does not currently feed it one.
  That is a gap, and it is named rather than papered over.
- A zeroed `MotionLimits` is refused rather than turned into a plausible number.
  An unconfigured axis must not yield a stopping distance, for the same reason
  an unconfigured safety link must not yield a permit.
- Zero clearance permits zero speed, as a value and not an error. A guard flush
  against the hazard is a real thing to describe, and the absence of room is not
  permission to move.
