#!/bin/zsh
# Runs each journey once, each from a fresh launch, with cleanup after every run (failed ones too).
# Usage: .claude/skills/verify/scripts/journeys-all.sh [journey ...]
#        (default: record-pass edit export separation)
# Prints one line per journey and exits 1 when any journey or its launch failed.
# Waits up to 15 minutes for an Overdub it did not start to quit before each launch.
set -uo pipefail
SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
failed=0
(( $# )) || set -- record-pass edit export separation
for journey in "$@"; do
    for _ in {1..180}; do pgrep -x Overdub >/dev/null || break; sleep 5; done
    if ! launched="$("$SCRIPTS/launch.sh" "journey-$journey" 2>&1)"; then
        echo "$journey LAUNCH-FAILED: $launched"
        failed=1
        continue
    fi
    run_dir="$(grep '^RUN_DIR=' <<<"$launched" | cut -d= -f2-)"
    "$SCRIPTS/doctor.sh" "$run_dir" >"$run_dir/doctor.txt" 2>&1
    doctor=$?
    "$SCRIPTS/journey-$journey.sh" "$run_dir" >"$run_dir/journey-stdout.txt" 2>&1
    code=$?
    "$SCRIPTS/cleanup.sh" "$run_dir" >"$run_dir/cleanup.txt" 2>&1
    result="$(tail -1 "$run_dir/journeys/$journey/checks.txt" 2>/dev/null || echo "RESULT none")"
    echo "$journey exit=$code doctor=$doctor $result $run_dir"
    [[ $code -eq 0 && $doctor -eq 0 ]] || failed=1
done
exit $failed
