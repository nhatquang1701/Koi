#!/bin/bash
# CCRL 1000-game ultra-fast crash/legality test on the Mac.  Pauses the Lichess
# bot between games (documented heavy-work placeholder + autostart), rebuilds the
# current sources, runs Koi vs the local Stockfish 19 oracle for 1000 games at
# 5 ms/move, then always restarts the bot.
set -u
unset CDPATH
REPO="$HOME/koi-head"
BOT="$HOME/koi-bot/koi-bot.sh"
NET="$HOME/koi-bot/engines/koi.nnue"
SF="$HOME/koi-run/sf19/stockfish/stockfish-macos-universal"
OPENINGS="$REPO/tests/data/openings/openings-curated-32.txt"
OUT="$HOME/koi-ccrl-20261101"
PLACEHOLDER_PID=""
PAUSED=0
mkdir -p "$OUT"
echo "=== CCRL run start $(date) ===" | tee -a "$OUT/run.log"

restore_bot() {
    if [ -n "$PLACEHOLDER_PID" ] && kill -0 "$PLACEHOLDER_PID" 2>/dev/null; then
        kill "$PLACEHOLDER_PID" 2>/dev/null || true
        wait "$PLACEHOLDER_PID" 2>/dev/null || true
    fi
    if [ "$PAUSED" = "1" ]; then
        "$BOT" start >/dev/null 2>&1 || true
        sleep 5
        if pgrep -f "lichess-bot" >/dev/null 2>&1; then
            echo "BOT_RESTORED=yes $(date)" | tee -a "$OUT/run.log"
        else
            echo "BOT_RESTORED=NO_WARNING $(date)" | tee -a "$OUT/run.log"
        fi
    fi
}
trap restore_bot EXIT

# 1. Pause the bot (only between games; autostart refuses during a game).
nohup bash -c 'exec -a koi-bench-placeholder sleep 7200' >/dev/null 2>&1 &
PLACEHOLDER_PID=$!
sleep 1
for attempt in $(seq 1 60); do
    "$BOT" autostart >/dev/null 2>&1 || true
    if ! pgrep -f "lichess-bot" >/dev/null 2>&1; then
        PAUSED=1
        break
    fi
    sleep 10
done
if [ "$PAUSED" != "1" ]; then
    echo "PAUSE_FAILED: bot still running after 10 minutes; aborting" | tee -a "$OUT/run.log"
    exit 2
fi
echo "BOT_PAUSED=yes $(date)" | tee -a "$OUT/run.log"

# 2. Build the current sources and run the harness.
cd "$REPO" || exit 1
export KOI_NNUE_PATH="$NET"
echo "=== build $(date) ===" | tee -a "$OUT/run.log"
if ! cmake --build build/release --target koi_engine --parallel 6 > "$OUT/build.log" 2>&1; then
    echo "BUILD_FAILED" | tee -a "$OUT/run.log"
    tail -40 "$OUT/build.log" | tee -a "$OUT/run.log"
    exit 3
fi
shasum -a 256 build/release/koi-engine "$NET" | tee -a "$OUT/run.log"
echo "=== harness $(date) ===" | tee -a "$OUT/run.log"
python3 "$HOME/koi-ccrl-crash_1000.py" \
    --engine "$REPO/build/release/koi-engine" \
    --opponent "$SF" \
    --openings "$OPENINGS" \
    --games 1000 --workers 6 --movetime 5 \
    --out "$OUT/1000-games.json" > "$OUT/1000-games.log" 2>&1
HARNESS_EXIT=$?
echo "HARNESS_EXIT=$HARNESS_EXIT" | tee -a "$OUT/run.log"
tail -30 "$OUT/1000-games.log" | tee -a "$OUT/run.log"
echo "=== CCRL run done $(date) ===" | tee -a "$OUT/run.log"
exit "$HARNESS_EXIT"
