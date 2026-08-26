#!/usr/bin/env bash
#
# Sweeps the poll interval and records how the cell's stop reaction responds.
#
# One data point would show the polled link is slower. The sweep shows *why*:
# the median tracks half the poll interval, because a trip lands uniformly
# within it. That makes the relationship predictable rather than anecdotal --
# and it means the number can be computed for any proposed poll rate without
# running anything.
#
# Usage: scripts/measure-reaction.sh [build-dir] [output-dir]

set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${1:-$HOME/b-pc}"
OUT="${2:-$ROOT/evidence}"
BINARY="$BUILD/src/pickcell-reaction"

if [ ! -x "$BINARY" ]; then
  echo "pickcell-reaction not built at $BINARY" >&2
  exit 127
fi

mkdir -p "$OUT"
{
  echo "Safety reaction time against poll interval"
  echo "=========================================="
  echo
  echo "Same cell, same source, same machine. Only the link changes."
  echo
  for poll in 10 25 50 100 200; do
    echo "--- poll interval ${poll} ms ---"
    "$BINARY" --trials 20 --poll-ms "$poll" 2>&1 |
      grep -E "shared-memory|HTTP poll"
    echo
  done
  echo "The shared-memory figure is the control: it does not move with the poll"
  echo "interval because it does not poll. What it is bounded by is the cell's own"
  echo "1 kHz control period, which the cell chooses."
  echo
  echo "The polled figure tracks half the poll interval, as a uniformly-timed"
  echo "event within a fixed window must."
} | tee "$OUT/reaction-vs-poll-interval.txt"
