# pickcell

A pick-and-place cell assembled from the other repositories — and a measurement
of the thing that assembling them exposes.

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![License](https://img.shields.io/badge/license-Apache--2.0-green)

[`motionkit`](https://github.com/Onwcan/motionkit) owns the frames and the pose
arithmetic. [`robot-contracts`](https://github.com/Onwcan/robot-contracts) owns
the wire format. [`safeedge`](https://github.com/Onwcan/safeedge) decides whether
torque is permitted. This repository is what happens when you put them together
and then ask a question none of them can answer alone.

**All three are fetched from their published repositories, not from sibling
directories.** That is a test in itself: it consumes them exactly the way a third
party would, so a broken export or a target name that only exists in-tree fails
here rather than in someone else's build.

---

## The question

The safety runtime decides to stop the machine. How long until the cell actually
stops?

Not a property of the safety runtime. safeedge reacts in about a millisecond and
says so on its own dashboard. The cell reacts within one control period of
learning. **Neither component is slow, and the system can still take a tenth of
a second to stop**, because the answer lives in the link between them.

## The measurement

`pickcell-reaction` runs both link implementations against one source, in one
process, on one machine. The source stamps the instant it decides; both links
carry that stamp through unchanged, so what comes out is the true
decision-to-reaction interval rather than a round-trip time.

Medians and maxima, milliseconds — from
[`evidence/reaction-vs-poll-interval.txt`](evidence/reaction-vs-poll-interval.txt):

| poll interval | shared memory p50 | shared memory max | HTTP poll p50 | HTTP poll max |
|---:|---:|---:|---:|---:|
| 10 ms | 0.33 | 1.07 | 6.4 | **10.7** |
| 25 ms | 0.54 | 0.95 | 15.5 | **25.8** |
| 50 ms | 0.56 | 1.02 | 25.5 | **43.5** |
| 100 ms | 0.44 | 1.05 | 64.9 | **96.1** |
| 200 ms | 0.37 | 1.09 | 122.0 | **191.0** |

**The HTTP maximum equals the poll interval.** 10.7 against 10, 25.8 against 25,
96.1 against 100, 191 against 200. That is not a coincidence needing explanation
— it is the definition. An event occurring at a uniformly random moment inside a
fixed window is discovered, at worst, a full window later. So the worst-case
stop time can be **computed** for any proposed poll rate without measuring
anything.

**The shared-memory column does not move.** It sits near 0.4 ms at the median
and 1.0 ms at the maximum in every row, because it does not poll. What bounds it
is the cell's own 1 kHz control period: the cell reads the link once per cycle,
so the reaction is uniform in [0, 1 ms] and the link itself contributes almost
nothing.

Which gives the result its useful form:

> With shared memory the bound is **the cell's control period, which the cell
> chooses.** With polling the bound is **the poll interval, which is usually
> chosen by whoever was worried about load.**

The trip instant is randomised within the poll window, because tripping at a
fixed offset would phase-lock with the poller and report whichever point of the
interval was picked — a number that could be made to say anything.

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
```

```bash
./build/src/pickcell-reaction --trials 30 --poll-ms 100
```

---

## The gap this exposed

safeedge's `/readyz` returns 200 or 503 and nothing else. No timestamp for the
decision.

That makes the end-to-end reaction time **unmeasurable** across it. A reader can
only stamp its own observation, so any interval it computes comes out near zero —
which does not mean the link is fast, it means there is nothing to measure.
`HttpPollSafetyLink::Format::kReadinessProbe` implements exactly that and says so
at the definition.

This is why `robot-contracts` puts `monotonic_ns` on `StampedPose` and
`since_monotonic_ns` on `SafetyState`. **A signal that cannot be timed cannot be
held to a deadline.** A readiness probe is a liveness answer being asked to do a
safety job.

---

## The cell

Six-axis arm, a bin of parts, a fixture. The layout is a motionkit frame tree —
`world → base → flange → tcp`, with `bin` and `fixture` as furniture fixed in the
world — so "where is the tool relative to the fixture" is a query rather than a
hand-written chain of multiplications. The arm is bolted down a quarter-turn off
the world axes, which is exactly the kind of detail a frame graph exists to stop
anyone having to remember.

Status is published as `robot.v1.MotionStatus`, so the cell speaks the same wire
format as everything else in the column.

Two behaviours worth pointing at:

**No safety information means do not move.** Not "assume permitted". A cell that
has never heard from the safety runtime sits in `held_by_safety`, and
`pickcell_cycles_without_safety` is exported so a dead link shows on the
dashboard instead of looking like a quiet machine.

**A hold resumes where it interrupted.** A cell that restarts its cycle on every
safety event drops the part it was holding.

---

## Running the whole column

```bash
docker compose -f deploy/docker-compose.yml up --build
```

safeedge on `:9100`, the cell on `:9200`, Prometheus on `:9090`, Grafana on
`:3000`. Prometheus scrapes at 1 s rather than the usual 15 s — a safety
transition and the cell's reaction to it are tens of milliseconds apart, and a
15 s scrape puts both in one sample and shows nothing. It is still far too
coarse to *measure* the reaction; that is what `pickcell-reaction` is for.

The compose stack deliberately uses the HTTP link, because that is how these
systems are normally assembled and this repository is about being honest
regarding what it costs. The trade is real in both directions: HTTP works across
machines, survives either end restarting, needs no shared IPC namespace, and can
be inspected with `curl` by someone who has never seen the code. Shared memory
gives all that up to reach a sub-millisecond bound.

The rule is not "always use shared memory". It is that **the safety link's worst
case has to be a number someone chose on purpose**, sized against the machine's
stopping distance — rather than whatever fell out of a default poll interval.

Verified: the stack comes up, Prometheus scrapes both targets, and stopping the
safety runtime holds the cell (see below).

---

## The defect the stack found

Bringing the column up end to end found a real bug in this repository, and it is
the one worth reading.

With everything running, `docker compose stop safety-runtime` — and the cell
**kept working**. Cycles advanced from 5 to 7 while every safety poll failed:

```
pickcell_cycles_completed 7          <-- advanced by two
pickcell_held_by_safety 0            <-- still not held
pickcell_safety_poll_failures 2
```

`HttpPollSafetyLink::poll()` re-stamped `observed_monotonic_ns` on every call,
so a cached "torque permitted" looked freshly confirmed forever. The safety
runtime was gone and the cell was moving on a permit nobody had reissued.
**Absence of information reading as permission** — precisely the failure
direction `robot-contracts` ADR-0002 argues must never exist, reintroduced two
repositories later by a single line.

The fix: the link reports when its information was *obtained*, not when it was
asked for, and a permit expires. After it, the same experiment freezes the cell:

```
pickcell_cycles_completed 2          <-- frozen, and still frozen 6 s later
pickcell_held_by_safety 1
pickcell_cycles_on_stale_safety 7656
```

`CellSafety.StopsBelievingAPermitThatHasGoneStale` is the regression test.
[`evidence/stale-permit-defect.txt`](evidence/stale-permit-defect.txt) has both
runs.

Two things this is worth noting for. A unit test would not have found it —
every unit test passed both before and after, because the bug only exists when a
real dependency stops answering. And **`SharedMemorySafetyLink` still has the
same exposure**: if the writer dies the seqlock keeps its last value and the
read succeeds regardless. Closing that needs a writer heartbeat, which is what
the sequence number in safeedge's safety telegram is for. Recorded rather than
quietly left out.

---

## Further reading

[`docs/adr/0001`](docs/adr/0001-how-the-cell-learns-about-safety.md) — the full
argument, the trade in both directions, and why a software link however fast is
not a safety-rated stop.

---

## Licence

Apache-2.0. Portfolio work.
