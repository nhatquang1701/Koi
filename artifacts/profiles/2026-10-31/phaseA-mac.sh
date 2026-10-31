#!/bin/bash
# Koi profiling phase A: verify build/net, bench timing probe, depth/NPS reality runs.
set -u
unset CDPATH
cd "$HOME/koi-head/build/release" || { echo "missing koi-head build tree"; exit 1; }
NET="$HOME/koi-bot/engines/koi.nnue"
OUT="${1:-$HOME/koi-prof-20261031}"
mkdir -p "$OUT"
export KOI_NNUE_PATH="$NET"

echo "=== host ==="
date
sysctl -n machdep.cpu.brand_string 2>/dev/null
echo "ncpu=$(sysctl -n hw.ncpu)"
echo "=== binaries ==="
shasum -a 256 koi-engine koi-bench "$NET"
ls -la koi.nnue 2>/dev/null || echo "no koi.nnue beside binary"
echo "=== handshake ==="
printf 'uci\nquit\n' | ./koi-engine 2>&1 | grep -E 'id name|info string build|info string NNUE|EvalFile' | head -5

uci_run() {
    local name="$1" threads="$2" fen="$3" limit="$4" hold="$5"
    local inp="$OUT/$name.in"
    mkdir -p "$OUT/logs"
    printf 'uci\nisready\nsetoption name Threads value %s\nsetoption name Hash value 512\nisready\nposition fen %s\n' "$threads" "$fen" > "$inp"
    printf '%s\n' "$limit" >> "$inp"
    { cat "$inp"; sleep "$hold"; printf 'stop\nquit\n'; } | ./koi-engine > "$OUT/logs/$name.log" 2>&1
    echo "--- $name threads=$threads limit=[$limit] ---"
    grep -E '^info depth' "$OUT/logs/$name.log" | tail -3
    grep -E '^bestmove' "$OUT/logs/$name.log" | tail -1
}

SP='rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1'
MID='r1bqkbnr/pppp1ppp/2n5/4p3/4P3/5N2/PPPP1PPP/RNBQKB1R w KQkq - 2 3'

echo "=== bench fen-file probe depth 6 (3 positions) ==="
START=$(date +%s)
./koi-bench --nnue "$NET" --threads 1 --depth 6 --fen-file "$OUT/fens-small.txt" > "$OUT/logs/bench-probe-d6.log" 2>&1
RC=$?
ELAPSED=$(( $(date +%s) - START ))
echo "bench_probe depth6 rc=$RC elapsed_s=$ELAPSED"
tail -8 "$OUT/logs/bench-probe-d6.log"

echo "=== depth/NPS reality: startpos ==="
uci_run t1-startpos-mt1000 1 "$SP" 'go movetime 1000' 3
uci_run t1-startpos-mt5000 1 "$SP" 'go movetime 5000' 7
uci_run t1-startpos-clock1s 1 "$SP" 'go wtime 1000 btime 1000' 3
uci_run t4-startpos-mt1000 4 "$SP" 'go movetime 1000' 3
uci_run t4-startpos-mt5000 4 "$SP" 'go movetime 5000' 7

echo "=== node efficiency: fixed depth 8 ==="
uci_run t1-startpos-d8 1 "$SP" 'go depth 8' 10
uci_run t1-mid-d8 1 "$MID" 'go depth 8' 12
uci_run t4-startpos-d8 4 "$SP" 'go depth 8' 10

echo "=== middlegame movetime 5000 ==="
uci_run t1-mid-mt5000 1 "$MID" 'go movetime 5000' 7
uci_run t4-mid-mt5000 4 "$MID" 'go movetime 5000' 7

echo "=== PHASE A DONE ==="
