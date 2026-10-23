# Sourced by the journey-*.sh scripts. Not run on its own.
# Every request, reply and check of a journey is written under $EVIDENCE ($RUN_DIR/journeys/<name>/):
#   requests.jsonl  one {"request": [...], "reply": {...}} line per overdub-ctl call
#   checks.txt      one "ok"/"FAIL" line per check, then "RESULT pass|fail"
#   state-*.json    app state snapshots taken by snapshot
# A journey exits 1 on the first failed check (after recording it) and 0 when every check passed.
set -uo pipefail
setopt null_glob
SCRIPTS="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPTS/../../../.." && pwd)"
RUN_DIR="$(cd "${RUN_DIR:-$REPO/build/verify/latest}" && pwd -P)"
SOCKET="$(cat "$RUN_DIR/socket")"
CTL="$SCRIPTS/overdub-ctl"
# song::minVolumeDb, the Master fader's bottom (a gain, not silence).
MASTER_MIN_DB=-60

journey_begin() {
    EVIDENCE="$RUN_DIR/journeys/$1"
    rm -rf "$EVIDENCE"
    mkdir -p "$EVIDENCE"
    : >"$EVIDENCE/checks.txt"
    : >"$EVIDENCE/requests.jsonl"
    echo "journey $1: evidence in $EVIDENCE"
}

journey_fail() {
    echo "FAIL $1" | tee -a "$EVIDENCE/checks.txt"
    echo "RESULT fail" >>"$EVIDENCE/checks.txt"
    echo "journey: FAILED, see $EVIDENCE" >&2
    exit 1
}

journey_end() {
    echo "RESULT pass" >>"$EVIDENCE/checks.txt"
    echo "journey: passed, see $EVIDENCE/checks.txt"
}

# ctl OP [key=value ...]: sends one request, logs it, prints the reply; a non-ok reply fails the journey.
ctl() {
    local reply code
    reply="$("$CTL" --socket "$SOCKET" "$@" 2>&1)"
    code=$?
    jq -cn --args '{request: $ARGS.positional}' "$@" | jq -c --arg reply "$reply" '. + {reply: ($reply | try fromjson catch .)}' >>"$EVIDENCE/requests.jsonl"
    [[ $code == 0 ]] || journey_fail "request $* answered: $reply"
    echo "$reply"
}

# state SECTION [SECTION ...]: prints the named app state sections as one JSON object.
state() {
    local parts
    parts="$(jq -cn --args '$ARGS.positional' "$@")"
    ctl state "parts=$parts" | jq -c 'del(.id, .ok, .seq)'
}

# snapshot NAME SECTION ...: saves the sections to state-NAME.json.
snapshot() {
    local name="$1"
    shift
    state "$@" | jq . >"$EVIDENCE/state-$name.json"
}

# check DESCRIPTION JSON JQ_FILTER: passes when the filter prints true for the JSON.
check() {
    if [[ "$(jq -r "$3" <<<"$2" 2>/dev/null)" == "true" ]]; then
        echo "ok   $1" | tee -a "$EVIDENCE/checks.txt"
    else
        echo "     got: $(jq -c "$3" <<<"$2" 2>&1 | head -c 400)" >>"$EVIDENCE/checks.txt"
        journey_fail "$1"
    fi
}

# check_cmd DESCRIPTION SHELL_CONDITION: passes when the condition exits 0.
check_cmd() {
    if eval "$2"; then
        echo "ok   $1" | tee -a "$EVIDENCE/checks.txt"
    else
        journey_fail "$1"
    fi
}

# Nothing may be heard: every journey sets the Master volume to its minimum before it plays or
# records, and no journey turns Monitoring on.
silence_master() {
    ctl set_master_volume "volume_db=$MASTER_MIN_DB" >/dev/null
    local s
    s="$(state song monitoring)"
    check "Master volume is at its minimum ($MASTER_MIN_DB dB)" "$s" ".song.song.master.volumeDb <= $MASTER_MIN_DB"
    check "no Track is monitored" "$s" '.monitoring == []'
}

# activity_started: records the number of the activity just started (the app numbers each from 1).
activity_started() {
    ACTIVITY_COUNTER="$(state activity | jq -r '.activity.counter')"
}

# wait_finish KIND TIMEOUT_MS: waits until the activity activity_started recorded finishes, then
# checks that it was KIND and ended without an error.
wait_finish() {
    ctl wait pointer=/activity/last_finish/counter "equals=$ACTIVITY_COUNTER" "timeout_ms=$2" >/dev/null
    local s
    s="$(state activity)"
    echo "$s" | jq . >"$EVIDENCE/state-finish-$1.json"
    check "activity $ACTIVITY_COUNTER finished as $1 with no error" "$s" ".activity.last_finish.kind == \"$1\" and .activity.last_finish.error_code == null"
}

# write_test_wav PATH SECONDS: a 16-bit stereo 44.1 kHz 110 Hz sine with a click every 0.5 s.
write_test_wav() {
    python3 -I - "$1" "$2" <<'PY'
import math, struct, sys, wave
sr, n = 44100, int(44100 * float(sys.argv[2]))
with wave.open(sys.argv[1], "wb") as w:
    w.setnchannels(2); w.setsampwidth(2); w.setframerate(sr)
    frames = bytearray()
    for i in range(n):
        s = 0.3 * math.sin(2 * math.pi * 110 * i / sr)
        if i % (sr // 2) < 200:
            s += 0.5 * math.exp(-(i % (sr // 2)) / 40)
        v = int(max(-1.0, min(1.0, s)) * 32767)
        frames += struct.pack("<hh", v, v)
    w.writeframes(bytes(frames))
PY
}
