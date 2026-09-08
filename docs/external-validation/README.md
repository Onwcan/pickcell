# External validation package

A frozen, pre-registered description of one bounded experiment, so that an
independent physics simulator can cross-check the physical consequence of a
reaction delay **without using this repository's code**.

Committed before any external result existed. That ordering is the point: a
prediction published after seeing the answer is not a prediction.

Nothing here names a particular collaborator. The package is an artifact of this
repository and should outlive any one collaboration.

---

## What this can establish, and what it cannot

**It can** check one thing: given a reaction time and a commanded braking law,
does the tool travel as far as this repository says? An independent integrator
disagreeing would be a real finding.

**It cannot** check the argument this repository actually makes:

1. This repository **measured** the communication and reaction behaviour itself
   — `evidence/reaction-vs-poll-interval.txt`,
   `evidence/end-to-end-stop-time.txt`.
2. An external run produces its **own** reaction time from its **own** simulated
   polling model. That is not this repository's Linux, HTTP or shared-memory
   implementation, and running it says nothing about them.
3. So the cross-check tests the **physical consequence** of a reaction delay,
   conditional on whatever delay the simulator observes. Both sides report a
   reaction time; **neither confirms the other's.**
4. **Stopping distance is the primary result.** Clearance and collision are
   arithmetic against one frozen hazard plane. Clearance moves with the distance
   and is informative; collision is a coarse verdict. Neither is a separate
   independent validation.

**This is not a safety certification** and nothing here should be read as one.

---

## The experiment: one factor

The independent variable is the **stop-signal polling interval**. Everything else
— initial state, limits, commanded braking law — is identical in every run.

| polling interval | 10, 25, 50, 100, 200 ms |
|---|---|
| initial velocity | 2.0 m/s |
| initial acceleration | **0.0 m/s²** |
| limits | 2.0 m/s, 8.0 m/s², 40.0 m/s³ |
| hazard plane | 0.700 m |
| degrees of freedom | 1, along `+x` |

The simulator reports the reaction time **it** observes in each trial, along with
stopping distance, clearance and crossing state.

### `T` is defined at the profile boundary, not at signal observation

The formula below is only exact if `T` is the duration of the constant-speed
phase. So:

```
T = t_profile_start − t_hazard_assertion
```

| | |
|---|---|
| `t_hazard_assertion` | simulation timestamp at which the stop condition becomes true |
| `t_profile_start` | simulation timestamp of the **first integration step that applies the frozen braking jerk** — the step at which commanded jerk becomes −40 m/s³ |

Where a simulator distinguishes *command issuance*, *command activation*, and
*the first integration step using the new value*, it is the **third** that
defines the boundary. A simulator can observe the stop signal on one tick and not
apply braking until a later one, and **that interval belongs inside `T`**.

`t_stop_signal_observed` is requested too, but it is **diagnostic** — not
necessarily the endpoint of `T`. With all three timestamps, `T` decomposes:

```
signal_detection_latency    = t_stop_signal_observed − t_hazard_assertion
control_application_latency = t_profile_start        − t_stop_signal_observed
T                           = their sum
```

### The hazard phase is frozen too

For a fixed polling interval, the reaction time depends on where the hazard lands
relative to the polling schedule. A hazard always asserted at the same absolute
instant would phase-lock differently against different intervals and report
artifacts of the chosen origin. This repository's own benchmark randomises the
trip within the window for that reason.

The external schedule is **pre-registered** rather than randomised — not because
seeded randomness would be irreproducible, which it would not be. The reasons are
narrower: a fixed list depends on no RNG implementation or seed-handling
convention, every trial is enumerable before anything runs, an outside party can
reproduce and audit the schedule by reading it, and the identical
nuisance-variable schedule applies across every polling interval.

```
hazard assertion = phase_fraction × polling_interval, after a poll boundary
phase_fraction   = frac(k / φ),  k = 1..5
```

Both the **generator** and the **concrete values** are recorded in
`scenario-v1.json`, which is authoritative — the values are emitted at
round-trip-safe precision (`std::numeric_limits<double>::max_digits10`), so a
reader parses back exactly the doubles this implementation used, and reproduction
does not depend on anyone else's approximation of φ. Approximately: 0.6180,
0.2361, 0.8541, 0.4721, 0.0902.

A low-discrepancy sequence covers the window evenly from five points without
clustering, and its generator is not a simple fraction of the interval the way
`{0.1, 0.3, 0.5, 0.7, 0.9}` would be. That **reduces** the chance of landing on a
timestep boundary; it does not eliminate it, since any floating-point value is
rational.

There is a second reason to sweep phase: the prediction is evaluated at each
run's own measured `T`, so a spread of phases produces a **spread of `T` values**.
Without it the ladder could return one nearly identical value per interval, and
the linear prediction would be tested at five points instead of across its range.

**5 intervals × 5 phases = 25 trials.** Bounded, and every condition gets the
same phase set.

If the simulator can report the tick or timestamp of the poll boundary preceding
each hazard assertion, the realised phase can be audited:

```
observed_phase = (t_hazard_assertion − t_preceding_poll_boundary) / polling_interval
```

That is provenance and diagnostics — **not** an acceptance gate on the physics.
same phase set.

### The prediction is a function, not a number

This package does **not** predict the reaction time. Deriving it from the polling
interval would assert exactly the relationship a measurement exists to establish.
What is pre-registered is the physical consequence, conditional on whatever the
simulator reports:

```
total_stopping_distance(T) = 2.0 m/s × T + 0.450 m
```

The braking term is constant across every run because the state at brake onset is
the frozen state however long the signal took to arrive.

The hazard plane is reached when `T` exceeds **0.125 s**, which falls inside the
range this ladder produces — so the crossing verdict genuinely varies rather than
reading the same value in every row.

### The hazard plane exerts no force

It is **purely observational**: zero force, zero collision response. This is not
a detail. The quantity being cross-checked is the **free** stopping distance
under the commanded braking law, and a rigid collider at 0.700 m would apply a
contact impulse that alters the very trajectory the comparison is about.

Realise it however the simulator prefers — a trigger volume, a sensor boundary
with collision response disabled, or post-processing an unobstructed trajectory.
"Crossing" is a verdict *about* a trajectory, not an event *in* it.

### Crossing is classified symbolically, not by a constant

The exact boundary is `total_stopping_distance = 0.700 m`, which for the
analytical prediction occurs at `T = 0.125 s`. The classification inherits the
same distance tolerance `delta_m` that gate 2 uses:

| condition | verdict |
|---|---|
| `stop_position < 0.700 − delta_m` | CLEAR |
| `stop_position > 0.700 + delta_m` | CROSSED |
| otherwise | INDETERMINATE |

An earlier draft froze a 2 ms band here. That was inconsistent: it invented a
numeric constant while the numerical tolerance was deliberately left open until
the simulator's parameters are known. The verdict should carry the declared
uncertainty, not one of its own.

---

## The commanded braking law

Recovered from the generated profile by locating the instants at which commanded
jerk changes — **not restated from a formula**. The closed form
`v·a/(2j) + v²/(2a)` only holds where the stop reaches the acceleration limit,
and below `max_acceleration² / max_jerk` (1.6 m/s here) it does not.

| segment | duration | jerk | entry v | entry a |
|---|---|---|---|---|
| 0 | 0.200 s | −40 m/s³ | 2.000 m/s | 0 |
| 1 | 0.050 s | 0 | 1.200 m/s | −8 m/s² |
| 2 | 0.200 s | +40 m/s³ | 0.800 m/s | −8 m/s² |

Total 0.450 s, 0.450 m.

### Why 2.0 m/s

The cell's own 0.5 m/s transfer speed is *below* the threshold, so its stop never
reaches the acceleration limit and its middle segment collapses to zero duration.
A state above the threshold gives three non-degenerate segments — harder to
reproduce by accident, easier to reproduce wrongly, so a mismatch carries
information. `ValidationPackage.ASlowerStateWouldNotHaveGivenThatStructure`
pins the contrast.

### Why zero initial acceleration

Because it is what the cell does. `Cell::step` advances the tool at a constant
transfer speed along straight-line segments and never ramps.

**This is a modelling choice for this experiment, not a claim that safety events
occur at zero acceleration.** They do not. `motionkit::StopProfile` already
accepts an arbitrary initial state and reports the extra travel an accelerating
axis commits to; this package deliberately does not exercise that. A non-zero
initial acceleration is a *different experiment* and would be a new version.

---

## Reproducing the prediction

A commit SHA of this repository does **not** by itself identify the stopping law,
because the library that computes it is fetched. So the dependencies are pinned
to exact revisions in `CMakeLists.txt`, and those same variables are handed to
the code as compile definitions — the revisions the package reports cannot drift
from the ones actually built.

`GIT_SHALLOW` is deliberately *not* set on them. A shallow fetch of an arbitrary
commit does work against GitHub and was verified to work, but CMake documents it
as depending on server support and its behaviour has moved between versions. For
the one thing here that must resolve identically on someone else's machine, a
full clone of three small repositories is the cheaper trade.

```bash
cmake --build build --target pickcell_validation_package
./build/src/pickcell-validation-package docs/external-validation
```

---

## Version lifecycle

Two different checks, and the distinction is the whole design:

- The **active** version is compared against the generator, so drift is caught.
- **Archived** versions are compared against a recorded digest and *never*
  against the generator.

That second rule is what makes "add a new version" executable rather than
advisory. After an intended change to the calculation, an old prediction is still
what was published; it must not be rewritten to agree with new behaviour. A
single test comparing every version against current behaviour would make the
policy impossible — the old version would fail forever.

So when behaviour intentionally changes: **archive the outgoing version**
(record its files and digest in `archivedVersions()`), raise `kActiveVersion`,
and generate the new one beside it. The outgoing files are never touched.

Integrity is a **SHA-256 manifest**, `MANIFEST.sha256`, in the standard
`sha256sum` format. Anyone can verify the package they were sent with one
command and without building anything:

```bash
cd docs/external-validation && sha256sum -c MANIFEST.sha256
```

It covers **README.md as well as the data files**, because this document carries
frozen semantics — what the experiment can and cannot establish, the gate
definitions, the hazard-plane rules, the disclosure protocol. A manifest
advertised as standalone verification that omitted them would be verifying the
numbers and not their meaning. The manifest does not hash itself.

CI runs exactly that, and additionally fails if an artifact exists that the
manifest does not list — an artifact nobody recorded is one nobody can verify.

It lives outside the C++ runtime deliberately: a file check is no reason to pull
a cryptographic dependency into a library, and an external party should not have
to compile this repository to confirm the bytes. Git history remains the actual
publication record; the manifest makes accidental or silent modification fail
loudly.

---

## The agreement rule, fixed in advance

### Gate 1 — the same experiment

A **semantic** gate. It establishes that both sides ran the same experiment, not
that they arrived at the same numbers. It asks for confirmation of the degrees of
freedom and direction, the initial state, the limits, the commanded segment
durations and jerks, the force assumptions, and that velocity was held constant
during the reaction interval.

It explicitly does **not** require the simulator's integrated velocity trace to
reproduce the reference trace. Numerical deviation in the integrated solution is
*part of what this cross-check measures*; making it a precondition would discard
the result being sought.

If gate 1 fails, a different experiment was run — the distance comparison is not
meaningful, and the mismatch is the finding.

### Gate 2 — distance

Per run, against the formula evaluated at that run's reported reaction time.
Tolerance is the sum of:

- **stop-threshold residual** — from the table in `prediction-v1.json`, at
  whatever velocity threshold the simulator used. Measured from the generated
  profile (74.5 µm at 10 mm/s, 2.36 µm at 1 mm/s), not from a
  constant-deceleration tail approximation, because the tail of a jerk-limited
  stop has acceleration going to zero along with velocity;
- **numerical solution error** — **left open.** A per-step position error
  proportional to velocity times timestep is a heuristic for a first-order
  explicit scheme, not a bound for an arbitrary integrator. It is **not zero
  because gate 1 passed**: agreeing on the experiment says nothing about the
  numerical error in solving it;
- **reaction-time resolution** — velocity times the quantisation of the reported
  reaction time, since the prediction is evaluated at that reported value.

### The ordering is enforced by a two-phase protocol

Asking for solver settings and measured distances in one exchange, and *then*
claiming the tolerance was fixed before the distances were seen, is a
contradiction. So the exchange is split.

**Phase A — metadata only.** Simulator version and exact revision; physics
timestep; substeps; solver/integrator identification and relevant settings; the
stopped-velocity threshold and its semantics; the resolution at which reaction
time is reported and whether values are rounded, truncated or taken directly
from ticks; and which of *issuance / activation / first integration step* their
`t_profile_start` corresponds to. Timestamps or tick indices let `T` be
*recomputed* from the simulation timeline rather than trusted as a rounded
scalar — that makes the external simulator's own measurement auditable, and says
nothing about this repository's Linux/HTTP/shared-memory implementation.

**Then `delta_m` is instantiated and frozen**, and committed here in its own
commit before any measured distance is received. Git history then carries the
ordering — the one part of this protocol that cannot be asserted after the fact.

**Phase B — measured outputs.** Per trial, keyed by interval and phase fraction:
`t_hazard_assertion`, `t_stop_signal_observed`, `t_profile_start`, the resulting
`T`, stopping distance, clearance, crossing state; where practical the preceding
poll-boundary tick for the phase audit; optionally a velocity trace; and enough
configuration to reproduce the run.

If the experiment has already been run, the outputs should be **withheld** until
the freeze is acknowledged. Having the numbers in hand is not the problem; our
seeing them before the criterion is fixed is.

Outside the instantiated sum is a **discrepancy requiring investigation** — not
proof that either model is wrong. It could arise from modelling assumptions,
numerical integration, experiment semantics, configuration, or a defect on either
side.

### Diagnostic, not a gate

If the simulator can return its own velocity trace, comparing it against
`braking-reference-trace-v1.csv` localises *where* a distance discrepancy arises
rather than only that one exists. It carries its own tolerance, instantiated from
the same parameters. A deviation there is information about the integrator and
does not on its own invalidate a run.

---

## Workflow

1. This package is committed, with dependencies pinned.
2. The commit SHA is sent to the external party, who can verify the artifacts
   with `sha256sum -c MANIFEST.sha256`. The SHA identifies content; what
   establishes *ordering* is the message carrying it, since a commit date is
   settable and a hash is not a timestamp.
3. They simulate independently.
4. **Phase A**: their simulator metadata arrives — no measured outputs.
5. `delta_m` is instantiated, frozen, and committed here in its own commit.
6. **Phase B**: their measured outputs arrive and are compared.

No external simulator is a dependency of this repository, and no code is shared
in either direction. Same frozen inputs, two independent implementations, then a
comparison — coupling them would destroy the only thing it is for.
