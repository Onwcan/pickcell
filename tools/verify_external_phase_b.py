#!/usr/bin/env python3
"""Recompute the external phase B result from the raw held measurement.

This does not run the external simulator and does not reconstruct the
simulation. It reads one file of measured numbers and the frozen artifacts that
were published before those numbers existed, and it recomputes every derived
quantity from scratch: reaction time, stopping distance, the prediction, the
residual, the gate 2 verdict, the hazard classification and the summary
statistics. Then it checks that the committed result layer says the same thing.

The point is that nobody has to trust the collaborator's arithmetic, or ours.
The measured numbers and the run's provenance come from the external party and
are taken as given; every quantity DERIVED from them is recomputed here rather
than read out of their report.

The acceptance criterion is read out of the frozen files rather than written
here, so editing a frozen file to move the criterion breaks the pinned digests
below, and editing the result to match a different criterion breaks the
recomputation.

Standard library only, no network, no build. Exit status is 0 when every check
passes and 1 otherwise.
"""

from __future__ import annotations

import argparse
import csv
import hashlib
import json
import math
import pathlib
import sys

VERSION = "1.0.0"

# Digests published before the measured numbers were received. The first five
# are the pre-registration and the tolerance freeze; the sixth is the raw held
# measurement as it arrived, which must never be rewritten. A mismatch here is a
# finding, not something to regenerate.
PINNED_SHA256 = {
    "docs/external-validation/README.md":
        "8adc751d43eb93f12cb2116b1dbb900bb7bdd26ed6b8a435516c0ee10da19ace",
    "docs/external-validation/scenario-v1.json":
        "61954a2b0f8d6195421e49f95f676e447e662a793faa540ccbc9c780280e6bb9",
    "docs/external-validation/prediction-v1.json":
        "f767a0d65c7f2ffa8562bb60a3ff2858dd9fe1afb61f4eba573f4fd931001a7a",
    "docs/external-validation/braking-reference-trace-v1.csv":
        "167b45ee8cd8954fee0695acf3865797d50a6efd7cb25ca0db19ac1a699321ab",
    "docs/external-validation/tolerance-v1.json":
        "8bc831503dbeb2457e2d0c7acb507d128ee91dbff46162e2b1358d203e9fea1b",
    "docs/external-validation/results-v1/phase-a-timing.json":
        "bbde701d3a02cda4b9a43233c90488e8ef8be237e69352ca591c451b8486b9e4",
    "docs/external-validation/results-v1/phase-b-raw-held.json":
        "4562559ce13bb8ed51ba9ff9e21baeed5d77d665ec788aa953b0b162fe9a3a22",
}

# The timing that phase A published while the measurements were still withheld.
# Continuity means phase B reports these same values and adds the measured ones,
# rather than a fresh run that happens to be labelled the same way.
PHASE_A_FIELDS = (
    "poll_interval_ms",
    "phase_index",
    "frozen_phase_fraction",
    "requested_hazard_offset_s",
    "realized_hazard_tick_from_preceding_poll",
    "realized_phase_fraction",
    "trial_global_start_tick",
    "t_hazard_assertion_tick",
    "t_stop_signal_observed_tick",
    "t_profile_start_tick",
    "t_hazard_assertion_s",
    "t_stop_signal_observed_s",
    "t_profile_start_s",
)

# The eight fields phase A deliberately withheld and phase B released.
WITHHELD_UNTIL_PHASE_B = frozenset({
    "t_stop_tick",
    "reaction_time_T_s",
    "hazard_position_m",
    "stop_position_m",
    "stopping_distance_m",
    "clearance_m",
    "crossed_plane_without_tolerance",
    "stop_velocity_mps",
})

PREREGISTRATION_COMMIT = "cabace5a2c02da4ebbef3494b54ce87b324c348f"
TOLERANCE_COMMIT = "1490238eb35e4b4dfce115c428b7d5907eadf58a"

# Two kinds of comparison, and the difference matters.
#
# Values that were derived once and then written out are compared EXACTLY. JSON
# and the CSV both store floats as round-trip-safe decimal, so a double written
# and read back is bit-identical, and recomputation applies the same operations
# to the same inputs. A tolerance there would only hide drift -- the whole point
# is to notice when a committed number stops matching its derivation.
#
# The epsilon is for the few checks that compare across DIFFERENT expressions
# for the same quantity, where the last bit may legitimately differ: the
# collaborator wrote the requested hazard offset as phase * (poll_ms / 1000.0)
# and this file derives it as phase * (poll_ms * timestep).
EPSILON = 1e-12


class Report:
    """Collects pass/fail lines so every check runs before the exit status."""

    def __init__(self) -> None:
        self.failures: list[str] = []
        self.checks = 0

    def check(self, ok: bool, label: str, detail: str = "") -> bool:
        self.checks += 1
        if not ok:
            self.failures.append(label + (" -- " + detail if detail else ""))
        return ok

    def section(self, title: str) -> None:
        print("\n== %s ==" % title)

    def line(self, ok: bool, text: str) -> None:
        print("  [%s] %s" % ("ok" if ok else "FAIL", text))


def sha256_of(path: pathlib.Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def close(a: float, b: float, eps: float = EPSILON) -> bool:
    """Across-expression agreement: the last bit may legitimately differ."""
    return math.isclose(a, b, rel_tol=0.0, abs_tol=eps)


def exact(a: float, b: float) -> bool:
    """Round-tripped agreement: a committed value against its derivation."""
    return a == b


def verify_manifest(root: pathlib.Path, manifest: pathlib.Path, rep: Report) -> None:
    """sha256sum -c, in Python, so the check runs the same way everywhere."""
    entries = []
    for raw_line in manifest.read_text(encoding="utf-8").splitlines():
        if not raw_line.strip():
            continue
        digest, _, name = raw_line.partition("  ")
        entries.append((digest.strip(), name.strip()))
    rep.check(bool(entries), "%s is empty" % manifest.relative_to(root))
    for digest, name in entries:
        target = manifest.parent / name
        ok = target.is_file() and sha256_of(target) == digest
        rep.line(ok, "%s %s" % (manifest.parent.relative_to(root) / name, digest[:12]))
        rep.check(ok, "manifest entry does not verify: %s" % name)
    # An artifact nobody recorded is an artifact nobody can verify.
    listed = {name for _, name in entries}
    present = {p.name for p in manifest.parent.iterdir()
               if p.is_file() and p.name != manifest.name}
    missing = sorted(present - listed)
    rep.line(not missing, "every artifact in %s other than %s is listed"
                          % (manifest.parent.relative_to(root), manifest.name))
    rep.check(not missing, "not listed in %s: %s" % (manifest.name, ", ".join(missing)))


def load_frozen(root: pathlib.Path) -> dict:
    """Read the acceptance criterion out of the frozen files, never from here."""
    base = root / "docs" / "external-validation"
    scenario = json.loads((base / "scenario-v1.json").read_text(encoding="utf-8"))
    prediction = json.loads((base / "prediction-v1.json").read_text(encoding="utf-8"))
    tolerance = json.loads((base / "tolerance-v1.json").read_text(encoding="utf-8"))
    pred = prediction["prediction_is_a_function_of_reaction_time"]
    hazard = tolerance["hazard_classification"]
    return {
        "scenario_id": scenario["scenario_id"],
        "version": scenario["version"],
        "initial_velocity_mps": pred["initial_velocity_mps"],
        "braking_distance_m": pred["braking_distance_m"],
        "braking_duration_s": pred["braking_duration_s"],
        "hazard_plane_m": scenario["hazard_plane"]["distance_m"],
        "polling_intervals_s": scenario["trial_schedule"]["polling_intervals_s"],
        "phase_fractions": scenario["trial_schedule"]["phase_fractions"],
        "trial_count": scenario["trial_schedule"]["trial_count"],
        "delta_m": tolerance["delta_m"]["value"],
        "timestep_s": tolerance["phase_a_disclosure"]["integration"]["basic_timestep_s"],
        "stop_threshold_mps":
            tolerance["phase_a_disclosure"]["stop_semantics"]["threshold_mps"],
        "clear_below_m": hazard["hazard_plane_m"] - tolerance["delta_m"]["value"],
        "crossed_above_m": hazard["hazard_plane_m"] + tolerance["delta_m"]["value"],
        "diagnostic_expectation_m": -0.001,
    }


def classify(distance: float, frozen: dict) -> str:
    if distance < frozen["clear_below_m"]:
        return "CLEAR"
    if distance > frozen["crossed_above_m"]:
        return "CROSSED"
    return "INDETERMINATE"


def recompute(trials: list[dict], frozen: dict, rep: Report) -> list[dict]:
    """Derive every gate quantity from the raw measured fields alone."""
    dt = frozen["timestep_s"]
    v0 = frozen["initial_velocity_mps"]
    profile_ticks = round(frozen["braking_duration_s"] / dt)
    frozen_phases = frozen["phase_fractions"]
    rows = []
    simultaneous = 0

    for t in trials:
        poll_ms = t["poll_interval_ms"]
        idx = t["phase_index"]
        tag = "poll=%sms phase=%d" % (poll_ms, idx)

        hazard_tick = t["t_hazard_assertion_tick"]
        observed_tick = t["t_stop_signal_observed_tick"]
        profile_tick = t["t_profile_start_tick"]
        stop_tick = t["t_stop_tick"]

        # Timing. T is defined at the profile boundary, not at observation.
        t_ticks = profile_tick - hazard_tick
        t_s = t_ticks * dt
        rep.check(exact(t_s, t["reaction_time_T_s"]),
                  "%s: T disagrees with the reported reaction time" % tag)
        rep.check(observed_tick >= hazard_tick,
                  "%s: the signal was observed before the hazard" % tag)
        rep.check(profile_tick >= observed_tick,
                  "%s: braking began before the signal was observed" % tag)
        rep.check(0 <= t_ticks < poll_ms,
                  "%s: T is not inside one polling interval" % tag)
        if observed_tick == profile_tick:
            simultaneous += 1

        # The braking profile is frozen, so its duration in ticks is too.
        rep.check(stop_tick - profile_tick == profile_ticks,
                  "%s: stop is %d ticks after profile start, expected %d"
                  % (tag, stop_tick - profile_tick, profile_ticks))

        # Phase realisation: ceiling to the first tick at or after the request.
        expected_request = frozen_phases[idx - 1] * (poll_ms * dt)
        expected_hazard_local = int(math.ceil(expected_request / dt - 1e-12))
        rep.check(close(expected_request, t["requested_hazard_offset_s"]),
                  "%s: requested hazard offset is not the frozen fraction times the interval" % tag)
        rep.check(expected_hazard_local == t["realized_hazard_tick_from_preceding_poll"],
                  "%s: realised hazard tick does not follow the disclosed ceiling rule" % tag)
        rep.check(close(t["realized_phase_fraction"],
                        t["realized_hazard_tick_from_preceding_poll"] / poll_ms),
                  "%s: realised phase fraction is not the tick over the interval" % tag)
        rep.check(frozen_phases[idx - 1] == t["frozen_phase_fraction"],
                  "%s: frozen phase fraction does not match the scenario" % tag)

        # Geometry. Distance travelled from the hazard assertion, never the
        # absolute coordinate: the axis is not reset between trials.
        distance = t["stop_position_m"] - t["hazard_position_m"]
        rep.check(exact(distance, t["stopping_distance_m"]),
                  "%s: stopping distance is not stop position minus hazard position" % tag)

        predicted = v0 * t_s + frozen["braking_distance_m"]
        signed = distance - predicted
        absolute = abs(signed)
        gate_2 = absolute <= frozen["delta_m"]
        clearance = frozen["hazard_plane_m"] - distance
        state = classify(distance, frozen)

        # The run declared a stop at a velocity threshold; check it held.
        rep.check(abs(t["stop_velocity_mps"]) <= frozen["stop_threshold_mps"],
                  "%s: final speed %g exceeds the declared stop threshold"
                  % (tag, abs(t["stop_velocity_mps"])))

        # No time series exists. Refuse to treat one as present if it appears.
        for key, value in t.items():
            rep.check(not isinstance(value, list),
                      "%s: field %s is a sequence; this run recorded no trace" % (tag, key))

        rows.append({
            "poll_interval_ms": poll_ms,
            "phase_index": idx,
            "frozen_phase_fraction": t["frozen_phase_fraction"],
            "realized_phase_fraction": t["realized_phase_fraction"],
            "preceding_poll_boundary_tick":
                hazard_tick - t["realized_hazard_tick_from_preceding_poll"],
            "t_hazard_assertion_tick": hazard_tick,
            "t_stop_signal_observed_tick": observed_tick,
            "t_profile_start_tick": profile_tick,
            "t_stop_tick": stop_tick,
            "reaction_time_T_s": t_s,
            "hazard_position_m": t["hazard_position_m"],
            "stop_position_m": t["stop_position_m"],
            "stopping_distance_m": distance,
            "predicted_stopping_distance_m": predicted,
            "signed_residual_m": signed,
            "absolute_residual_m": absolute,
            "gate_2_pass": gate_2,
            "clearance_m": clearance,
            "hazard_plane_state": state,
            "stop_velocity_mps": t["stop_velocity_mps"],
        })

    rep.line(simultaneous == len(trials),
             "observation and profile start share a tick in %d of %d trials"
             % (simultaneous, len(trials)))
    return rows


def same_value(a: object, b: object) -> bool:
    """Type-sensitive comparison of parsed values.

    Integer and floating-point types must remain the same, and floats are
    compared using their canonical Python representation, so different parsed
    binary64 values are rejected and -0.0 stays distinguishable from 0.0, which
    == would not.

    This is a semantic value-continuity check. It is NOT a claim that the
    numeric tokens have byte-identical textual spelling: two decimal spellings
    that parse to the same binary64 have the same repr and compare equal here.
    Byte-level preservation of the received evidence is established separately,
    by the pinned digests above and the .gitattributes -text rules.
    """
    if type(a) is not type(b):
        return False
    if isinstance(a, float):
        return repr(a) == repr(b)
    return a == b


def check_continuity(phase_a: list[dict], phase_b: list[dict], rep: Report) -> None:
    """Phase B must preserve the timing phase A disclosed, and add the held values.

    Phase A was published while every measured distance was still withheld, so
    its timing is on the record before the results were. Comparing the two here
    is what makes that check reproducible by anyone with the repository, rather
    than something done once on a maintainer's machine.

    What this proves, precisely: the phase B evidence preserves the phase
    A-disclosed timing metadata exactly under the comparison semantics in
    same_value(). It would detect a rerun or replacement only if that operation
    changed one of these fields. Identical timing metadata alone cannot
    distinguish a preserved run from a deterministic rerun under identical
    conditions, so the collaborator's statement that no rerun occurred remains
    provenance supplied by the external party. This check independently
    establishes continuity of the disclosed metadata, which is a different and
    weaker claim than proving no rerun happened.
    """
    key = lambda row: (row["poll_interval_ms"], row["phase_index"])
    a_by_id = {key(r): r for r in phase_a}
    b_by_id = {key(r): r for r in phase_b}

    rep.check(len(phase_a) == len(a_by_id), "phase A contains a duplicated trial identity")
    rep.check(len(phase_a) == len(phase_b),
              "phase A has %d rows, phase B has %d" % (len(phase_a), len(phase_b)))
    identities_match = set(a_by_id) == set(b_by_id)
    rep.check(identities_match, "phase A and phase B describe different trials")
    rep.line(identities_match, "%d phase A rows, same trial identities as phase B" % len(phase_a))

    compared = 0
    for identity in sorted(set(a_by_id) & set(b_by_id)):
        before, after = a_by_id[identity], b_by_id[identity]
        for field in PHASE_A_FIELDS:
            compared += 1
            rep.check(field in before and field in after,
                      "trial %s is missing %s" % (identity, field))
            rep.check(same_value(before.get(field), after.get(field)),
                      "trial %s: phase B changed %s from %r to %r"
                      % (identity, field, before.get(field), after.get(field)))
        # Phase B may only add. Dropping a phase A field, or smuggling in an
        # unexpected one, is as much a break in continuity as changing a value.
        added = set(after) - set(before)
        rep.check(not set(before) - set(after),
                  "trial %s: phase B dropped %s" % (identity, sorted(set(before) - set(after))))
        rep.check(added == WITHHELD_UNTIL_PHASE_B,
                  "trial %s: phase B adds %s, expected exactly the withheld fields"
                  % (identity, sorted(added)))

    rep.line(compared == len(PHASE_A_FIELDS) * len(phase_a),
             "%d phase A fields per trial compared exactly, %d comparisons"
             % (len(PHASE_A_FIELDS), compared))
    rep.line(True, "phase B adds only the %d withheld measurement fields"
                   % len(WITHHELD_UNTIL_PHASE_B))


def check_identities(rows: list[dict], frozen: dict, rep: Report) -> None:
    identities = [(r["poll_interval_ms"], r["phase_index"]) for r in rows]
    expected = {(round(p / frozen["timestep_s"]), k)
                for p in frozen["polling_intervals_s"]
                for k in range(1, len(frozen["phase_fractions"]) + 1)}
    rep.check(len(rows) == frozen["trial_count"],
              "expected %d trials, found %d" % (frozen["trial_count"], len(rows)))
    rep.check(len(set(identities)) == len(identities),
              "a (poll_interval_ms, phase_index) pair is duplicated")
    rep.check(set(identities) == expected,
              "trial identities are not the frozen Cartesian product")
    rep.line(set(identities) == expected,
             "%d trials, exact Cartesian product of %d intervals and %d phases"
             % (len(rows), len(frozen["polling_intervals_s"]), len(frozen["phase_fractions"])))


def summarise(rows: list[dict], frozen: dict) -> dict:
    signed = [r["signed_residual_m"] for r in rows]
    states = [r["hazard_plane_state"] for r in rows]
    crossed = [{"poll_interval_ms": r["poll_interval_ms"],
                "phase_index": r["phase_index"],
                "stopping_distance_m": r["stopping_distance_m"]}
               for r in rows if r["hazard_plane_state"] == "CROSSED"]
    return {
        "trial_count": len(rows),
        "gate_2_pass_count": sum(1 for r in rows if r["gate_2_pass"]),
        "gate_2_fail_count": sum(1 for r in rows if not r["gate_2_pass"]),
        "signed_residual_min_m": min(signed),
        "signed_residual_max_m": max(signed),
        "signed_residual_mean_m": sum(signed) / len(signed),
        "max_absolute_residual_m": max(r["absolute_residual_m"] for r in rows),
        "hazard_counts": {state: states.count(state)
                          for state in ("CLEAR", "INDETERMINATE", "CROSSED")},
        "crossed_trials": sorted(crossed, key=lambda c: (c["poll_interval_ms"],
                                                         c["phase_index"])),
        "diagnostic_expectation_m": frozen["diagnostic_expectation_m"],
        "delta_m": frozen["delta_m"],
    }


def check_csv(path: pathlib.Path, rows: list[dict], rep: Report) -> None:
    with path.open(newline="", encoding="utf-8") as handle:
        table = list(csv.DictReader(handle))
    rep.check(len(table) == len(rows),
              "the CSV has %d rows, the raw measurement has %d" % (len(table), len(rows)))
    by_id = {(r["poll_interval_ms"], r["phase_index"]): r for r in rows}
    for record in table:
        key = (int(record["poll_interval_ms"]), int(record["phase_index"]))
        row = by_id.get(key)
        if not rep.check(row is not None, "the CSV has a trial the raw file does not: %s" % (key,)):
            continue
        for field, value in record.items():
            expected = row[field]
            if isinstance(expected, bool):
                ok = value == str(expected)
            elif isinstance(expected, int):
                ok = int(value) == expected
            elif isinstance(expected, float):
                ok = exact(float(value), expected)
            else:
                ok = value == expected
            rep.check(ok, "CSV %s field %s is %r, recomputed %r" % (key, field, value, expected))


def check_summary(summary: dict, computed: dict, frozen: dict, rep: Report) -> None:
    rep.check(summary["scenario_id"] == frozen["scenario_id"], "summary scenario_id drifted")
    rep.check(summary["scenario_version"] == frozen["version"], "summary scenario_version drifted")
    rep.check(summary["preregistration_commit"] == PREREGISTRATION_COMMIT,
              "summary preregistration_commit drifted")
    rep.check(summary["tolerance_commit"] == TOLERANCE_COMMIT,
              "summary tolerance_commit drifted")
    rep.check(summary["velocity_trace_available"] is False,
              "the summary claims a velocity trace; this run recorded none")
    rep.check(summary["independently_recomputed"] is True,
              "the summary does not declare independent recomputation")
    rep.check(summary["gate_1_pass"] is True, "the summary gate 1 verdict drifted")
    rep.check(exact(summary["delta_m"], frozen["delta_m"]),
              "the summary tolerance is not the frozen tolerance")

    for field in ("trial_count", "gate_2_pass_count", "gate_2_fail_count"):
        rep.check(summary[field] == computed[field],
                  "summary %s is %r, recomputed %r" % (field, summary[field], computed[field]))
    for field in ("signed_residual_min_m", "signed_residual_max_m", "signed_residual_mean_m",
                  "max_absolute_residual_m", "diagnostic_expectation_m"):
        rep.check(exact(summary[field], computed[field]),
                  "summary %s is %r, recomputed %r" % (field, summary[field], computed[field]))
    rep.check(summary["hazard_counts"] == computed["hazard_counts"],
              "summary hazard counts are %r, recomputed %r"
              % (summary["hazard_counts"], computed["hazard_counts"]))
    rep.check(len(summary["crossed_trials"]) == len(computed["crossed_trials"]),
              "summary crossed-trial count drifted")
    for claimed, actual in zip(summary["crossed_trials"], computed["crossed_trials"]):
        rep.check(claimed["poll_interval_ms"] == actual["poll_interval_ms"]
                  and claimed["phase_index"] == actual["phase_index"]
                  and exact(claimed["stopping_distance_m"], actual["stopping_distance_m"]),
                  "summary crossed trial %r does not match recomputation %r" % (claimed, actual))
    # Gate 2 is the frozen absolute-residual rule and stays that way even though
    # the observed residuals came in far inside it.
    rep.check(computed["gate_2_fail_count"] == 0
              or summary["gate_2_overall_pass"] is False,
              "gate 2 is reported as passing while a trial failed recomputation")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", default=None,
                        help="repository root (default: the parent of tools/)")
    args = parser.parse_args()
    root = pathlib.Path(args.root).resolve() if args.root \
        else pathlib.Path(__file__).resolve().parent.parent
    results = root / "docs" / "external-validation" / "results-v1"
    rep = Report()

    print("verify_external_phase_b %s" % VERSION)
    print("repository root: %s" % root)

    rep.section("provenance: digests published before the measurements arrived")
    for relative, digest in PINNED_SHA256.items():
        path = root / relative
        actual = sha256_of(path) if path.is_file() else "<missing>"
        ok = actual == digest
        rep.line(ok, "%s %s" % (relative, actual[:12]))
        rep.check(ok, "%s is %s, published as %s" % (relative, actual, digest))

    rep.section("manifests")
    verify_manifest(root, root / "docs" / "external-validation" / "MANIFEST.sha256", rep)
    verify_manifest(root, results / "MANIFEST.sha256", rep)

    frozen = load_frozen(root)
    rep.section("frozen criterion, read out of the committed artifacts")
    print("  prediction  : %.9f * T + %.9f m"
          % (frozen["initial_velocity_mps"], frozen["braking_distance_m"]))
    print("  delta_m     : %.12f m" % frozen["delta_m"])
    print("  CLEAR below : %.12f m" % frozen["clear_below_m"])
    print("  CROSSED over: %.12f m" % frozen["crossed_above_m"])
    print("  stop thresh : %g m/s" % frozen["stop_threshold_mps"])

    raw = json.loads((results / "phase-b-raw-held.json").read_text(encoding="utf-8"))
    trials = raw["trials"]
    phase_a = json.loads(
        (results / "phase-a-timing.json").read_text(encoding="utf-8"))["timing_rows"]

    rep.section("phase A to phase B continuity")
    rep.check(phase_a is not trials, "phase A and phase B evidence are the same object")
    check_continuity(phase_a, trials, rep)

    rep.section("trial identities")
    check_identities(trials, frozen, rep)

    rep.section("per-trial recomputation")
    rows = recompute(trials, frozen, rep)
    rep.line(True, "recomputed timing, distance, prediction, residual, gate 2, "
                   "clearance and classification for %d trials" % len(rows))

    computed = summarise(rows, frozen)

    rep.section("compact table")
    check_csv(results / "phase-b-trials.csv", rows, rep)
    rep.line(True, "every CSV field agrees with recomputation")

    rep.section("committed result summary")
    summary = json.loads((results / "phase-b-summary.json").read_text(encoding="utf-8"))
    check_summary(summary, computed, frozen, rep)
    rep.line(True, "summary agrees with recomputation")

    rep.section("recomputed result")
    print("  gate 2       : %d pass, %d fail, of %d"
          % (computed["gate_2_pass_count"], computed["gate_2_fail_count"],
             computed["trial_count"]))
    print("  signed resid : min %.15f  max %.15f  mean %.15f"
          % (computed["signed_residual_min_m"], computed["signed_residual_max_m"],
             computed["signed_residual_mean_m"]))
    print("  max abs resid: %.15f m  (%.4f%% of delta_m)"
          % (computed["max_absolute_residual_m"],
             100.0 * computed["max_absolute_residual_m"] / frozen["delta_m"]))
    print("  hazard states: %s" % computed["hazard_counts"])
    for crossing in computed["crossed_trials"]:
        print("    CROSSED poll=%sms phase=%d distance=%.12f m"
              % (crossing["poll_interval_ms"], crossing["phase_index"],
                 crossing["stopping_distance_m"]))

    print("\n%d checks run" % rep.checks)
    if rep.failures:
        print("FAILED (%d):" % len(rep.failures))
        for failure in rep.failures:
            print("  - %s" % failure)
        return 1
    print("ALL CHECKS PASSED")
    return 0


if __name__ == "__main__":
    sys.exit(main())
