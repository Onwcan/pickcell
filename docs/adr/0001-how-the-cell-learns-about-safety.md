# ADR-0001: How the cell learns that it must stop

- **Status**: Accepted
- **Date**: 2026-08-26
- **Deciders**: Onur Can Urhan

## Context

The safety runtime decides that torque must be removed. The cell has to find
out. Between the decision and the cell acting on it sits a link, and the link's
properties — not the runtime's — determine how quickly the machine actually
stops.

This is easy to get wrong because both ends look fine in isolation. The safety
runtime reacts in about a millisecond and says so on its own dashboard. The cell
reacts within one control period of learning. Neither component is slow, and the
system can still take a tenth of a second to stop.

## Decision

Two link implementations behind one interface, and the choice made explicitly
per deployment rather than by default:

- **`SharedMemorySafetyLink`** — a seqlock in POSIX shared memory. Same-machine
  only.
- **`HttpPollSafetyLink`** — polls an HTTP endpoint on an interval. Works across
  machines.

The compose stack uses the HTTP link, because that is how these systems are
normally assembled and this repository is about being honest concerning what
that costs.

## The measurement

`pickcell-reaction` runs both links against one source, in one process, on one
machine. The source stamps the instant it decides, and both links carry that
stamp through unchanged, so what is reported is the true decision-to-reaction
interval and not a round-trip time.

The trip instant is randomised within the poll window. Tripping at a fixed
offset would phase-lock with the poller and report whichever point of the
interval happened to be chosen — a number that could be made to say anything.

From `evidence/reaction-vs-poll-interval.txt`, medians and maxima in
milliseconds:

| poll interval | shared memory p50 | shared memory max | HTTP p50 | HTTP max |
|---:|---:|---:|---:|---:|
| 10 ms | 0.33 | 1.07 | 6.4 | 10.7 |
| 25 ms | 0.54 | 0.95 | 15.5 | 25.8 |
| 50 ms | 0.56 | 1.02 | 25.5 | 43.5 |
| 100 ms | 0.44 | 1.05 | 64.9 | 96.1 |
| 200 ms | 0.37 | 1.09 | 122.0 | 191.0 |

Two things to read off it.

**The HTTP maximum equals the poll interval.** 10.7 against 10, 25.8 against 25,
96.1 against 100, 191.0 against 200. That is not a coincidence to be explained,
it is the definition: an event occurring at a uniformly random moment within a
fixed window is discovered, at worst, a full window later. The consequence is
that the worst-case stop time can be *computed* for any proposed poll rate
without measuring anything.

**The shared-memory figure does not move.** It stays near 0.4 ms at the median
and 1.0 ms at the maximum across every row, because it does not poll. What
bounds it is the cell's own 1 kHz control period — the cell samples the link
once per cycle, so the reaction is uniform in [0, 1 ms]. The link contributes
almost nothing.

That is the useful form of the result: **with shared memory the bound is the
cell's control period, which the cell chooses. With polling the bound is the
poll interval, which is usually chosen by whoever was worried about load.**

## Why the compose stack still uses HTTP

Because the trade is real in both directions, and pretending otherwise would
make this a worse demonstration.

The HTTP link works across machines, survives either end restarting, needs no
shared filesystem or IPC namespace, and can be inspected with `curl` by someone
who has never seen the code. Shared memory gives up all of that to reach a
sub-millisecond bound.

The right rule is not "always use shared memory". It is that **the safety link's
worst case has to be a number someone chose on purpose**, sized against the
machine's stopping distance — not whatever fell out of a default scrape interval.

## The gap this exposed in the interface

safeedge's `/readyz` returns 200 or 503 and nothing else. It carries no
timestamp for the decision.

That makes the end-to-end reaction time **unmeasurable** across it. A reader can
only stamp its own observation, so any interval it computes comes out near zero
— which does not mean the link is fast, it means there is nothing to measure.
`HttpPollSafetyLink::Format::kReadinessProbe` implements exactly this and says so
at the definition.

This is why `robot-contracts` puts `monotonic_ns` on `StampedPose` and
`since_monotonic_ns` on `SafetyState`. **A signal that cannot be timed cannot be
held to a deadline**, and a readiness probe is a liveness answer being asked to
do a safety job.

## A permit is a statement about a moment, not a standing grant

Added after the compose stack found the omission.

The link interface as first written reported *what* the safety runtime said and
not *when* it was heard. `HttpPollSafetyLink::poll()` stamped the observation
time on every call, including calls that returned a cached value, so a permit
received once looked freshly confirmed indefinitely. Stopping the safety runtime
left the cell completing pick-and-place cycles against a runtime that no longer
existed.

Two changes close it:

- The link reports when its information was **obtained**, so a cached value
  visibly ages.
- The cell holds once that age exceeds `Cell::Config::max_safety_age_ns`,
  default 250 ms against a 100 ms poll — long enough to survive one dropped
  request, short enough that a dead runtime stops the machine rather than being
  outlived by its own last answer.

`cycles_on_stale_safety` is counted separately from `cycles_without_safety`
because "never heard from the runtime" and "heard, but too long ago to believe"
have different causes and different fixes.

Worth being clear about what this says regarding testing. Every unit test passed
before the fix and after it, because the defect only exists when a real
dependency stops answering. It took bringing the actual stack up and then
breaking part of it on purpose.

## Closing the same gap on the shared-memory link

The section above ended by recording that `SharedMemorySafetyLink` still had
this exposure: a seqlock read succeeds whether the writer is alive or dead, so
the reader could not tell a standing verdict from an abandoned one. Now closed.

The writer stamps `published_monotonic_ns` on **every** publish, changed or not,
and `heartbeat()` restates the current verdict on a period shorter than the
reader's staleness budget. The reader reports that stamp, so the existing
`max_safety_age_ns` check covers both links with no extra logic — the age simply
stops advancing when the heartbeat does.

`DeadWriter.TheCellStopsWhenTheWritingProcessExits` forks a real writer process,
lets it heartbeat, and kills it. A thread pretending to be a dead writer would
not be the same experiment: a thread that stops publishing leaves an intact
process holding the mapping, while a process that exits leaves the region behind
with nothing on the other end. The test asserts the uncomfortable middle state
explicitly — after the writer is gone, the read still succeeds and still says
torque is permitted — and then that the cell stops believing it.

### The mistake worth recording

The first attempt reported the writer's publish stamp as the reader's
observation time, on the reasoning that both answer "how fresh is this". They do
not. The shared-memory writer stamps its publish at the same instant it decides,
so the reaction time — observation minus decision — collapsed to **0.1
microseconds**. A number that looks like a triumph and means the measurement is
broken.

Two questions, two fields:

| field | question | shm | HTTP |
|---|---|---|---|
| `observed_monotonic_ns` | when did *I* find out? | the read | the fetch |
| `published_monotonic_ns` | when was the writer last demonstrably alive? | the writer's stamp | the fetch |

They coincide for the HTTP link, because the server answering is simultaneously
the reader learning and the proof of life. They diverge for shared memory, and
that divergence is the whole reason the heartbeat is needed.

Verified by injecting the regression: restoring the collapsed version makes
`DeadWriter` fail. A test that cannot fail proves nothing.

## Consequences

- Both links are behind one interface, so a deployment changes one line.
- The cell treats "no safety information" as "do not move", never as permission,
  and now treats "old safety information" the same way.
  `cycles_without_safety` is exported so a dead link is visible on the dashboard
  rather than looking like a quiet machine.
- The shared-memory link is same-machine only. A cell distributed across
  machines cannot use it and must size its poll interval against its stopping
  distance instead.
- None of this is a substitute for a hardware safety chain. A software link,
  however fast, is not a safety-rated stop; it is the part of the system that
  decides how quickly the software notices.
