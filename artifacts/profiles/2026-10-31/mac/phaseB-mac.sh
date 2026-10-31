#!/bin/bash
# Koi profiling phase B: bench probe, sample() CPU profiles, wall-clock budgets.
set -u
unset CDPATH
cd "$HOME/koi-head/build/release" || { echo "missing koi-head build tree"; exit 1; }
NET="$HOME/koi-bot/engines/koi.nnue"
OUT="${1:-$HOME/koi-prof-20261031}"
mkdir -p "$OUT/logs" "$OUT/samples"
export KOI_NNUE_PATH="$NET"
SP='rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
MID='r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3'

echo "=== bench probes (3 positions, 1 thread) ==="
E6=0
for D in 5 6; do
    START=$(date +%s)
    ./koi-bench --nnue "$NET" --threads 1 --depth "$D" --fen-file "$OUT/fens-small.txt" > "$OUT/logs/bench-probe-d$D.log" 2>&1
    RC=$?
    E=$(( $(date +%s) - START ))
    if [ "$D" = 6 ]; then E6=$E; fi
    echo "bench_probe depth=$D rc=$RC elapsed_s=$E"
    tail -4 "$OUT/logs/bench-probe-d$D.log"
done

# Pick a full-suite depth/repeat pair that keeps the bench running ~40-50 s.
# The probe uses 3 positions; scale to the 64-position suite and add a safety factor.
BENCH_DEPTH=6
EST=$(awk -v e="$E6" 'BEGIN { printf "%d", (e * 64 / 3) * 1.5 + 1 }')
if [ "$EST" -gt 120 ]; then
    BENCH_DEPTH=5
    EST=$(( EST / 4 ))
fi
BENCH_REPEAT=$(awk -v e="$EST" 'BEGIN { r = int(40 / (e > 0 ? e : 1)); if (r < 1) r = 1; if (r > 12) r = 12; printf "%d", r }')
echo "bench_full_suite_estimate_s=$EST chosen_depth=$BENCH_DEPTH repeat=$BENCH_REPEAT"
echo "=== bench sample (depth $BENCH_DEPTH repeat $BENCH_REPEAT, threads 1) ==="
./koi-bench --nnue "$NET" --threads 1 --depth "$BENCH_DEPTH" --repeat "$BENCH_REPEAT" \
    --profile-json "$OUT/bench-t1-profile.json" > "$OUT/logs/bench-sample.log" 2>&1 &
BPID=$!
sleep 4
sample -mayDie "$BPID" 25 -file "$OUT/samples/bench-t1.sample.txt"
echo "bench_sample_rc=$? bench_pid_was=$BPID"
wait "$BPID"
echo "bench_finished_rc=$?"
tail -3 "$OUT/logs/bench-sample.log"

echo "=== UCI mysample: middlegame go infinite, Threads=1 ==="
{ printf 'uci\nisready\nsetoption name Threads value 1\nsetoption name Hash value 512\nisready\nposition fen %s\ngo infinite\n' "$MID"; sleep 40; printf 'stop\nquit\n'; } | ./koi-engine > "$OUT/logs/uci-t1-infinite.log" 2>&1 &
EPID=$!
sleep 6
sample -mayDie "$EPID" 25 -file "$OUT/samples/uci-t1-infinite.sample.txt"
echo "uci_t1_sample_rc=$?"
wait "$EPID"
grep -E '^info depth|^bestmove' "$OUT/logs/uci-t1-infinite.log" | tail -3

echo "=== UCI sample: middlegame go infinite, Threads=4 ==="
{ printf 'uci\nisready\nsetoption name Threads value 4\nsetoption name Hash value 512\nisready\nposition fen %s\ngo infinite\n' "$MID"; sleep 40; printf 'stop\nquit\n'; } | ./koi-engine > "$OUT/logs/uci-t4-infinite.log" 2>&1 &
EPID=$!
sleep 6
sample -mayDie "$EPID" 25 -file "$OUT/samples/uci-t4-infinite.sample.txt"
echo "uci_t4_sample_rc=$?"
wait "$EPID"
grep -E '^info depth|^bestmove' "$OUT/logs/uci-t4-infinite.log" | tail -3

echo "=== wall-clock budget runs (engine process time) ==="
wall_run() {
    local name="$1" threads="$2" fen="$3" limit="$4" hold="$5"
    { printf 'uci\nisready\nsetoption name Threads value %s\nsetoption name Hash value 512\nisready\nposition fen %s\n' "$threads" "$fen" > "$OUT/logs/$name.in"
      printf '%s\n' "$limit" >> "$OUT/logs/$name.in"
      cat "$OUT/logs/$name.in"; sleep "$hold"; printf 'stop\nquit\n'; } |
        /usr/bin/time -p ./koi-engine > "$OUT/logs/$name.log" 2> "$OUT/logs/$name.time"
    echo "--- $name threads=$threads [$limit] ---"
    tr '\n' ' ' < "$OUT/logs/$name.time"; echo
    grep -E '^info depth' "$OUT/logs/$name.log" | tail -2
    grep -E '^bestmove' "$OUT/logs/$name.log" | tail -1
}
wall_run w-t1-mt200 1 "$SP" 'go movetime 200' 2
wall_run w-t4-mt200 4 "$SP" 'go movetime 200' 2
wall_run w-t1-mt1000 1 "$SP" 'go movetime 1000' 3
wall_run w-t4-mt1000 4 "$SP" 'go movetime 1000' 3
wall_run w-t1-clock1s 1 "$MID" 'go wtime 1000 btime 1000' 3
wall_run w-t4-clock1s 4 "$MID" 'go wtime 1000 btime 1000' 3

echo "=== machine noise (top cpu) ==="
ps -Ao pcpu,comm | sort -rn | head -6
echo "=== load ==="
sysctl -n vm.loadavg
echo "=== PHASE B DONE ==="
