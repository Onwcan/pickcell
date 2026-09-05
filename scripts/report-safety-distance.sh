#!/usr/bin/env bash
#
# What the measured reaction time costs in metres.
#
# The other two scripts in this directory measure milliseconds. This one turns
# them into the number a cell is actually laid out with, by adding the part the
# cell cannot do instantly: however quickly it learns, the arm still has to
# decelerate, and motionkit computes that under the axis's jerk and acceleration
# limits.
#
# Deterministic -- it is arithmetic over figures already measured, not a fresh
# measurement -- so it needs no running daemons and is safe to run in CI.
#
# Usage: scripts/report-safety-distance.sh <build-dir> [output-dir]

set -euo pipefail

BUILD="${1:?build dir}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${2:-$ROOT/evidence}"

BIN="$BUILD/src/pickcell-safety-distance"
if [ ! -x "$BIN" ]; then
  BIN="$BUILD/pickcell-safety-distance"
fi
if [ ! -x "$BIN" ]; then
  echo "pickcell-safety-distance not found under $BUILD" >&2
  exit 1
fi

mkdir -p "$OUT"
"$BIN" | tee "$OUT/safety-distance.txt"
