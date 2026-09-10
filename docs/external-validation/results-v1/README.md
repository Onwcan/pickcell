# External validation — the result

An independent simulator was given the frozen scenario and asked one question:
given a reaction time `T` and the frozen commanded braking law, does the tool
travel as far as this repository predicted?

**It does.** Gate 1 passes, gate 2 passes on all 25 trials, and the residuals sit
where they were predicted to sit before anybody saw them.

This directory is the result layer. It does not amend the pre-registration or
the tolerance, and the commit that adds it changes neither. The five artifacts
one directory up still carry the digests they were published with.

## What was found

| | |
|---|---|
| Trials | 25 |
| Gate 1, same experiment | **PASS** |
| Gate 2, distance within `delta_m` | **PASS, 25 / 25** |
| Out-of-tolerance discrepancies | 0 |
| Signed residual | −0.001000694 to −0.000999176 m |
| Mean signed residual | −0.001000033 m |
| Worst absolute residual | 0.001000694 m, **33.4 % of `delta_m`** |
| Hazard states | 23 CLEAR, 0 INDETERMINATE, **2 CROSSED** |

Both crossings are in the 200 ms polling condition, at `T = 0.152 s` and
`T = 0.181 s`.

Two kinds of input meet here, and it is worth keeping them apart.

The **measured evidence** — the per-trial positions, ticks and stop velocities —
and the **provenance** around it, such as the simulator version and the
statement that the run was not repeated, come from the external party. They are
taken as given; this repository cannot derive them from anything it holds.

Every **derived** quantity is recomputed rather than trusted. Reaction time,
stopping distance, the prediction, the residual, the gate 2 verdict, the
clearance, the hazard classification and every summary statistic in this
directory are produced from the raw measurement and the frozen artifacts by
[`tools/verify_external_phase_b.py`](../../../tools/verify_external_phase_b.py).
The collaborator's own summary, report and classified table were read only to
check that they agreed with that recomputation — they did — and are not the
source of a derived value here.

## The order things happened in

This is the part that cannot be reconstructed afterwards, so it is worth stating
plainly.

1. **The prediction was published first**, at commit `cabace5a`, as a function
   rather than a number: `total_stopping_distance(T) = 2.0·T + 0.450 m`.
2. **Phase A disclosed the simulator and withheld the measurements.** Integrator,
   solver, timestep, stop semantics, timing representation and per-trial timing
   ticks — but no distance, no stop position, no clearance, no verdict.
3. **The tolerance was frozen next**, at commit `1490238e`:
   `delta_m = 0.003000074536 m`, derived from that disclosure and committed on
   its own before any measured distance was received.
4. **Phase B was released only then.** The 25 trials had already been run and
   the outputs were held until the tolerance commit existed. That they were not
   repeated or adjusted afterwards is the external party's statement; what this
   repository can check independently is the continuity of the disclosed
   metadata, below.
5. **This layer evaluates already-held data against an already-frozen
   criterion.**

Git history fixes the public pre-registration and tolerance-freeze ordering —
steps 1, 3 and 5, which are commits in this repository. That Phase A was
disclosed between steps 1 and 3, and that Phase B was released after the
tolerance freeze, are documented by the preserved external evidence and
correspondence rather than by git history alone. Committing the Phase A timing
file is what puts the first of those on the public record here; the second rests
on the collaborator's account of when they sent it.

## Phase B preserves the timing Phase A disclosed

Step 2 put the per-trial timing on the record while every measured distance was
still withheld. Both files are committed here, and the verifier compares them on
every CI run: **13 Phase A fields × 25 trials = 325 comparisons**, all of which
must match. Phase B must also add **exactly** the eight fields that were
withheld — `t_stop_tick`, `reaction_time_T_s`, `hazard_position_m`,
`stop_position_m`, `stopping_distance_m`, `clearance_m`,
`crossed_plane_without_tolerance`, `stop_velocity_mps` — dropping none and
introducing none.

All 325 comparisons match. Because the Phase A file is committed, that is a
check you can rerun, not a claim about something done once on a maintainer's
machine.

**What that does and does not prove.** The continuity check proves that the
Phase B evidence preserves the Phase A-disclosed timing metadata exactly under
the verifier's comparison semantics. It would detect a rerun or replacement only
if that operation changed one of those committed fields. Identical timing
metadata alone cannot distinguish a preserved run from a deterministic rerun
under identical conditions. The collaborator's no-rerun statement therefore
remains provenance supplied by the external party, while the field-by-field
comparison independently establishes continuity of the disclosed metadata.

**How values are compared.** Continuity uses type-sensitive comparison of parsed
values. Integer and floating-point types must remain the same, and
floating-point values are compared using their canonical Python representation,
so different parsed binary64 values are rejected and cases such as `-0.0` versus
`0.0` remain distinguishable. This is a semantic value-continuity check, not a
claim that the numeric tokens have byte-identical textual spelling — two decimal
spellings that parse to the same binary64 compare equal here. Byte-level
preservation of the received evidence files is established separately, by their
pinned SHA-256 digests and the `.gitattributes -text` rules.

## The residuals landed where they were told to

Before Phase B was disclosed, `tolerance-v1.json` recorded a signed diagnostic
expectation of **−0.001 m** — not a gate, deliberately falsifiable. It came from
the disclosed integrator: MuJoCo's Euler is semi-implicit, so position advances
with the already-updated velocity, and for this profile on this grid the discrete
travel is exactly `trapezoid(v) + (h/2)·(v_N − v_0)`. The trapezoidal term
cancels because the profile spends 200 intervals at −40 m/s³ and 200 at +40, and
what survives is half a timestep of the total velocity change: −1 mm exactly.

Observed: every one of the 25 residuals is negative, the mean is
**−0.001000033 m**, and the largest departure from −1 mm is **0.82 µm**.

The observed scatter is fully consistent with float32 position quantization. All
50 stored position values are exactly representable as float32 values, and every
departure from the pre-registered −1 mm diagnostic expectation is smaller than
the float32 spacing at the corresponding absolute coordinate. No larger
unexplained deviation is evident.

The axis is never reset between trials, so the absolute coordinate climbs to
about 15.6 m by the end of the ladder, where the float32 spacing is about
9.5 × 10⁻⁷ m — against a worst observed departure of 8.2 × 10⁻⁷ m. That
establishes consistency rather than exclusivity: it does not rule out other
sub-ULP contributions, only that nothing larger than float32 position
quantization needs to be invoked to account for what was observed. It is also
one reason the classification is done on **stopping distance** rather than on
the absolute coordinate.

The gate was **not** tightened to match. `delta_m` is what it was on 1490238e.

## Why two trials crossed

The frozen plane sits 0.700 m from the hazard assertion. With the tolerance
applied, CROSSED requires more than 0.703000074536 m, which the prediction
reaches at `T = 0.1265 s`; the analytical no-tolerance boundary is `T = 0.125 s`.

Both crossings are 200 ms trials with `T` far past that — 0.152 s and 0.181 s —
and every CLEAR trial sits below it, the nearest being 200 ms phase 4 at
`T = 0.105 s`. Nothing lands near the band, which is why no trial is
INDETERMINATE. This is a sanity check on the classification, not a replacement
for it: the frozen rule classifies on distance, and that is what was used.

## What this does not show

The run produced its own reaction times from its own simulated polling model.
That is not this repository's Linux, HTTP or shared-memory implementation, so
this result says nothing about those, and neither side's reaction-time
measurement confirms the other's. Stopping distance is the primary result;
clearance and crossing are arithmetic against the frozen plane.

**None of this is safety certification.** It is not ISO conformance, not
functional-safety compliance, and not a claim about any physical machine. It is
one bounded physics-side cross-check of the consequence of a reaction delay.

## The missing trace

The external run recorded a final stop velocity per trial but **captured no
time-series velocity trace**, and none was reconstructed after the fact.

The optional diagnostic trace comparison is therefore unavailable. That is
missing diagnostic evidence, not a failed gate: gate 1 is semantic and gate 2 is
about distance, and neither ever depended on a trace. Had a discrepancy appeared,
the trace would have helped localise it — there is no discrepancy to localise.

## What is in here, and what is not

| file | what it is |
|---|---|
| `phase-a-timing.json` | the Phase A timing, byte-for-byte as received **before** Phase B existed |
| `phase-b-raw-held.json` | the measurement, byte-for-byte as received — the canonical evidence |
| `phase-b-trials.csv` | the same 25 trials as a table, byte-for-byte as received |
| `phase-b-summary.json` | the independently recomputed result, plus a full inventory of both archives |
| `MANIFEST.sha256` | digests for every result artifact in this directory other than itself |

`phase-a-timing.json` is here so the continuity check above is something you can
run, not something you have to take our word for. It came from
`pickcell_phase_a.zip` (28 092 bytes, SHA-256 `1a32b40b…d7741`) and was not
regenerated or reserialised.

**What is retained, and why.** `phase-b-raw-held.json` is the canonical raw
measurement: every derived quantity here is recomputed from it, and nothing else
in the archive can stand in for it. `phase-b-trials.csv` is *semantically
derivable* from those same trials, so keeping it is not strictly necessary to
reproduce the result — it is kept because it is an independently received
serialisation of the same data, which lets the verifier check the two
representations against each other as well as against recomputation. The two
files therefore encode overlapping trial information by design.

The original archive is `pickcell_phase_b.zip`, 37 766 bytes, SHA-256
`da3bd40dcd01ebe22a282287dc87def85d3540a368ab425130fabbc1767e9b38`. It is **not**
committed. Of its twelve files, five are the collaborator's copies of artifacts
already in this repository, and three more — their classified table, report and
summary — are values the verifier recomputes from the raw measurement. Those are
inventoried by size and SHA-256 in `phase-b-summary.json` rather than duplicated
in full, so the original stays verifiable by anyone who obtains it.

All three committed evidence files arrived with CRLF line endings and their
digests were published over those exact bytes, so `.gitattributes` marks those
three paths `-text`. Without that, git's `text=auto eol=lf` rule would normalise
them on commit and silently break the digests this result rests on.

## Checking it yourself

```sh
sha256sum -c docs/external-validation/MANIFEST.sha256
sha256sum -c docs/external-validation/results-v1/MANIFEST.sha256
python3 tools/verify_external_phase_b.py
```

The verifier needs no build, no network and no simulator. It reads the committed
evidence and the frozen artifacts, checks Phase A → Phase B continuity,
recomputes reaction time, stopping distance, the prediction, the residual, the
gate 2 verdict, the classification and every summary statistic, and fails if any
of them disagrees with what is committed. The acceptance criterion is read out of
the frozen files rather than written into the verifier, so moving the criterion
breaks the pinned digests and moving the result breaks the recomputation.

Everything it needs is in this repository. Nothing here depends on holding a copy
of either archive, on network access, or on the external simulator.
