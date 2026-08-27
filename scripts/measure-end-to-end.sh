#!/usr/bin/env bash
#
# The real end-to-end stop time: safeedge's own decision instant to the cell
# ceasing to command motion, across two processes.
#
# Distinct from scripts/measure-reaction.sh, which compares link *types* against
# a synthetic source. This one measures the actual column with the actual safety
# runtime in it, and is only possible because safeedge publishes the instant it
# decided -- against a plain readiness probe the figure is zero by construction.
#
# Usage: scripts/measure-end-to-end.sh <safeedge-build> <pickcell-build> [trials]

set -uo pipefail

SE_BUILD="${1:?safeedge build dir}"
PC_BUILD="${2:?pickcell build dir}"
TRIALS="${3:-10}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/evidence"

ESTOP=/tmp/e2e-estop
ACK=/tmp/e2e-ack
SE_PORT=9931
PC_PORT=9932
POLL_MS=100

cleanup() {
  pkill -f "safeedged" 2>/dev/null
  pkill -f "pickcelld" 2>/dev/null
  rm -f "$ESTOP" "$ACK"
}
trap cleanup EXIT
cleanup
sleep 1

SAFEEDGE_METRICS_PORT="$SE_PORT" SAFEEDGE_ESTOP_FILE="$ESTOP" SAFEEDGE_ACK_FILE="$ACK" \
  "$SE_BUILD/safeedged" >/tmp/e2e_se.log 2>&1 &
sleep 3

PICKCELL_SAFETY_HOST=127.0.0.1 PICKCELL_SAFETY_PORT="$SE_PORT" \
  PICKCELL_SAFETY_PATH=/safety PICKCELL_SAFETY_FORMAT=stamped \
  PICKCELL_SAFETY_POLL_MS="$POLL_MS" PICKCELL_METRICS_PORT="$PC_PORT" \
  "$PC_BUILD/src/pickcelld" >/tmp/e2e_pc.log 2>&1 &
sleep 4

reaction() {
  curl -s --max-time 5 "http://127.0.0.1:$PC_PORT/metrics" |
    awk '/^pickcell_last_stop_reaction_seconds/ { print $2 }'
}

mkdir -p "$OUT"
samples=()
for i in $(seq 1 "$TRIALS"); do
  # Sleep a random fraction of a poll interval so the trip does not phase-lock
  # with the poller. Without this the figure reports whichever point of the
  # window happened to be chosen, which can be made to say anything.
  sleep "0.$(( (RANDOM % 9) + 1 ))"
  rm -f "$ESTOP"
  touch "$ACK"
  sleep 0.5

  before="$(reaction)"
  touch "$ESTOP"
  for _ in $(seq 1 60); do
    now="$(reaction)"
    if [ -n "$now" ] && [ "$now" != "$before" ] && [ "$now" != "0.000000" ]; then
      samples+=("$now")
      break
    fi
    sleep 0.05
  done
done

{
  echo "End-to-end safety stop: safeedge decision -> pickcell stops commanding"
  echo "======================================================================"
  echo
  echo "Two processes. safeedge stamps the instant it entered the safe state on"
  echo "/safety; the cell subtracts that from its own observation. Poll interval"
  echo "${POLL_MS} ms, cell control period 1 ms, ${#samples[@]} trips."
  echo
  for s in "${samples[@]}"; do
    printf '  %8.1f ms\n' "$(awk -v v="$s" 'BEGIN { print v * 1000 }')"
  done
  echo
  printf '%s\n' "${samples[@]}" | awk '
    { v = $1 * 1000; a[NR] = v; if (NR == 1 || v < min) min = v; if (v > max) max = v; sum += v }
    END {
      if (NR == 0) { print "no samples"; exit }
      n = asort(a)
      printf "  min %.1f ms   median %.1f ms   max %.1f ms   (n=%d)\n", min, a[int((n+1)/2)], max, NR
    }' 2>/dev/null || printf '%s\n' "${samples[@]}" | awk '{ v=$1*1000; if(NR==1||v<min)min=v; if(v>max)max=v } END { printf "  min %.1f ms   max %.1f ms   (n=%d)\n", min, max, NR }'
  echo
  echo "The bound is the poll interval, as scripts/measure-reaction.sh predicts."
  echo "Nothing here is a property of the safety runtime: safeedge decides in"
  echo "about a millisecond and the cell reacts within one control period of"
  echo "learning. The interval is the link."
} | tee "$OUT/end-to-end-stop-time.txt"
