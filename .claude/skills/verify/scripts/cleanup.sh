#!/bin/zsh
# Quits the Overdub this skill started (the pid launch.sh recorded), never one found by name.
# Usage: .claude/skills/verify/scripts/cleanup.sh [run-dir]   (default: build/verify/latest)
# Keeps the run dir: it is the evidence. Copies this launch's JSONL log into it as overdub.jsonl.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
RUN_DIR="$(cd "${1:-$REPO/build/verify/latest}" && pwd -P)"
PID="$(cat "$RUN_DIR/pid" 2>/dev/null || true)"

if [[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null; then
    # The quit Apple Event runs JUCE's normal shutdown (finishes a running Pass, logs app.stopped).
    # launch.sh refused to start while another Overdub ran, so the bundle id names only our pid.
    osascript -e 'tell application id "com.mbarbieri.overdub" to quit' >/dev/null 2>&1
    for _ in {1..20}; do kill -0 "$PID" 2>/dev/null || break; sleep 0.5; done
    if kill -0 "$PID" 2>/dev/null; then
        echo "cleanup: pid $PID did not quit in 10 s (a modal dialog or a running Separation refuses quit); sending SIGKILL" >&2
        kill -KILL "$PID"
    fi
    echo "cleanup: stopped Overdub pid $PID"
else
    echo "cleanup: no running Overdub for $RUN_DIR"
fi
if [[ -f "$RUN_DIR/log-path" ]]; then
    cp "$(cat "$RUN_DIR/log-path")" "$RUN_DIR/overdub.jsonl" 2>/dev/null
fi
rm -f "$RUN_DIR/pid"
echo "EVIDENCE=$RUN_DIR"
