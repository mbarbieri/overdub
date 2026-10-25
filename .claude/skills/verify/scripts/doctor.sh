#!/bin/zsh
# Read-only: is the Overdub in build/verify/latest worth driving?
# Usage: .claude/skills/verify/scripts/doctor.sh [run-dir]
# Exit 0 when every check passes, 1 otherwise. Prints one "ok"/"FAIL" line per check.
set -uo pipefail
setopt null_glob
SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPTS/../../../.." && pwd)"
RUN_DIR="${1:-$REPO/build/verify/latest}"
# Resolved, as the process reports it (build/ may be a symlink).
APP_BIN="$(cd "$REPO/build/debug/src/app/Overdub_artefacts/Debug/Overdub.app/Contents/MacOS" 2>/dev/null && pwd -P)/Overdub"
failed=0
check() { if eval "$2"; then echo "ok   $1"; else echo "FAIL $1"; failed=1; fi; }

PID="$(cat "$RUN_DIR/pid" 2>/dev/null || true)"
LOG="$(cat "$RUN_DIR/log-path" 2>/dev/null || true)"
check "run dir has a pid ($RUN_DIR)" '[[ -n "$PID" ]]'
check "pid $PID is alive" '[[ -n "$PID" ]] && kill -0 "$PID" 2>/dev/null'
check "pid $PID runs this checkout's debug build" '[[ "$(ps -o command= -p "$PID" 2>/dev/null)" == "$APP_BIN"* ]]'
check "exactly one Overdub process" '[[ "$(pgrep -x Overdub | wc -l | tr -d " ")" == 1 ]]'
check "log file exists ($LOG)" '[[ -n "$LOG" && -f "$LOG" ]]'
check "log git_sha matches HEAD $(git -C "$REPO" rev-parse --short=12 HEAD)" \
    'grep -q "\"git_sha\":\"$(git -C "$REPO" rev-parse --short=12 HEAD)" "$LOG" 2>/dev/null'
check "binary is newer than every src/ file" '[[ -z "$(find "$REPO/src" -newer "$APP_BIN" -type f -print -quit)" ]]'
check "audio device open" 'grep -q "\"event\":\"engine.device_opened\"" "$LOG" 2>/dev/null'
SOCKET="$(cat "$RUN_DIR/socket" 2>/dev/null || true)"
check "control socket answers hello ($SOCKET)" '[[ -n "$SOCKET" ]] && "$SCRIPTS/overdub-ctl" --socket "$SOCKET" hello >/dev/null'
check "no error-level log lines" '! grep -q "\"lvl\":\"error\"" "$LOG" 2>/dev/null'

if [[ -f "$LOG" ]]; then
    echo "--- device and notices from this launch:"
    grep -E '"event":"(engine\.device_opened|app\.device_[a-z_]+|ui\.notice_shown|ui\.alert_shown)"' "$LOG" \
        | jq -c '{event, device, sample_rate, code, reason}' 2>/dev/null
fi
exit $failed
