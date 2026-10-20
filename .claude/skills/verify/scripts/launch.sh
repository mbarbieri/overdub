#!/bin/zsh
# Builds the debug Overdub.app and starts it through LaunchServices for verification.
# Usage: .claude/skills/verify/scripts/launch.sh [run-name]
# Prints RUN_DIR, PID and LOG; the run dir is build/verify/<stamp>-<run-name>, linked as build/verify/latest.
# Refuses when any Overdub is already running: the app is single-instance and shares
# ~/Library/Application Support/Overdub (device choice, calibrations, model) with the user's copy.
set -euo pipefail
setopt null_glob
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
APP="$REPO/build/debug/src/app/Overdub_artefacts/Debug/Overdub.app"
RUN_NAME="${1:-run}"
RUN_DIR="$REPO/build/verify/$(date +%Y%m%d-%H%M%S)-$RUN_NAME"

if pgrep -x Overdub >/dev/null; then
    echo "launch: an Overdub process is already running (pids: $(pgrep -x Overdub | tr '\n' ' ')). Not starting a second one." >&2
    echo "launch: if build/verify/latest/pid names it, run cleanup.sh; otherwise ask the user to quit it." >&2
    exit 1
fi

cmake --build --preset debug --target Overdub >/dev/null
# The process reports its resolved path, so match against that (build/ may be a symlink).
APP="$(cd "$APP" && pwd -P)"
mkdir -p "$RUN_DIR/songs"
# A Unix socket path must fit in 104 bytes, so the socket lives in a short private folder under /tmp;
# $RUN_DIR/socket records where.
SOCKET="/tmp/ovd-$(date +%s)-$$/control.sock"
echo "$SOCKET" >"$RUN_DIR/socket"
ln -sfn "$RUN_DIR" "$REPO/build/verify/latest"

# `open` (not the bare binary) so the app is a LaunchServices app that answers the quit Apple Event.
open -n -g "$APP" --env OVERDUB_LOG=debug --env OVERDUB_CONTROL_SOCKET="$SOCKET" --stdout "$RUN_DIR/stdout.txt" --stderr "$RUN_DIR/stderr.txt"

PID=""
for _ in {1..20}; do
    PID=$(pgrep -f "^$APP/Contents/MacOS/Overdub" | head -1 || true)
    [[ -n "$PID" ]] && break
    sleep 0.5
done
if [[ -z "$PID" ]]; then
    echo "launch: Overdub did not start; see $RUN_DIR/stderr.txt" >&2
    exit 1
fi
echo "$PID" >"$RUN_DIR/pid"

# Ready = the main window opened an audio device (the saved one or the default fallback), or
# reported there is none (app.device_unavailable; the window is still usable), and the control
# socket listens.
LOG=""
READY=""
for _ in {1..60}; do
    logs=(~/Library/Logs/Overdub/overdub-*-"$PID".jsonl)
    LOG="${logs[-1]:-}"
    if [[ -n "$LOG" ]]; then
        READY=$(grep -oE '"event":"(engine\.device_opened|app\.device_unavailable)"' "$LOG" | head -1 || true)
        [[ -n "$READY" ]] && grep -q '"event":"control.server_listening"' "$LOG" && break
    fi
    if ! kill -0 "$PID" 2>/dev/null; then
        echo "launch: Overdub (pid $PID) exited during startup; see $RUN_DIR/stderr.txt" >&2
        exit 1
    fi
    sleep 0.5
done
if [[ -z "$READY" ]]; then
    echo "launch: pid $PID not ready after 30 s (log: ${LOG:-none})" >&2
    exit 1
fi
echo "$LOG" >"$RUN_DIR/log-path"
echo "RUN_DIR=$RUN_DIR"
echo "SOCKET=$SOCKET"
echo "PID=$PID"
echo "LOG=$LOG"
echo "READY=$READY"
