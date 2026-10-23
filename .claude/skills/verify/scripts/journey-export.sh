#!/bin/zsh
# Journey export: export the open Song as a WAV and as an M4A into $RUN_DIR/exports/ through the
# export form's Intents, then read each file with afinfo (format, rate, duration = the Song span).
# Usage: .claude/skills/verify/scripts/journey-export.sh [run-dir]   (default: build/verify/latest)
# Runs journey-record-pass.sh first when no Song is open. Evidence: $RUN_DIR/journeys/export/.
RUN_DIR="${1:-}"
source "$(dirname "$0")/journey-lib.sh"
journey_begin export
if [[ "$(state song | jq -r '.song == null')" == "true" ]]; then
    "$SCRIPTS/journey-record-pass.sh" "$RUN_DIR" || journey_fail "the setup journey-record-pass.sh failed"
fi

OUT="$RUN_DIR/exports"
mkdir -p "$OUT"
s="$(state song)"
RATE="$(jq -r '.song.song.sampleRate' <<<"$s")"
# The Song span: from 0 to the end of its last Clip. A Song with no Insert has no tail.
SPAN_SAMPLES="$(jq -r '[.song.song.tracks[].clips[] | .timelineStart + .length] | max' <<<"$s")"
check "the Song has Clips and no Insert" "$s" \
    "($SPAN_SAMPLES > 0) and ([.song.song.tracks[].insertChain[]] + .song.song.master.insertChain | length) == 0"
SPAN="$(jq -n "$SPAN_SAMPLES / $RATE")"
echo "song span: $SPAN_SAMPLES samples = $SPAN s at $RATE Hz" | tee "$EVIDENCE/span.txt"

ctl show_mode mode=export >/dev/null

# export_as FORMAT FILE: fills the form, starts the export and waits for it to finish.
export_as() {
    local file="$OUT/$2"
    rm -f "$file"
    ctl set_export_format "format=$1" >/dev/null
    ctl set_export_destination "path=$file" >/dev/null
    check "the export form says $1 to $file" "$(state view)" \
        '.view.export_form.format == $format and .view.export_form.destination == $file' --arg format "$1" --arg file "$file"
    silence_master
    ctl export >/dev/null
    activity_started
    wait_finish export 60000
    check_cmd "$2 exists with nonzero length" '[[ -s "$file" ]]'
    check_cmd "no .partial file is left in $OUT" '[[ -z "$(ls -a "$OUT" | grep -F .partial)" ]]'
    afinfo "$file" >"$EVIDENCE/afinfo-$2.txt" 2>&1
    cat "$EVIDENCE/afinfo-$2.txt" | grep -E 'File type ID|Data format|estimated duration'
}

# duration_close FILE TOLERANCE_S: afinfo printed a duration and it is the Song span within the tolerance.
duration_close() {
    awk -v span="$SPAN" -v tol="$2" '/estimated duration/ { f = 1; d = $3 - span; ok = (d <= tol && -d <= tol) } END { exit !(f && ok) }' "$EVIDENCE/afinfo-$1.txt"
}

export_as wav song.wav
check_cmd "afinfo: song.wav is a WAVE file" 'grep -q "File type ID:   WAVE" "$EVIDENCE/afinfo-song.wav.txt"'
check_cmd "afinfo: song.wav is 2 ch, $RATE Hz, 24-bit PCM" \
    'grep -qE "2 ch, +$RATE Hz, lpcm .*24-bit" "$EVIDENCE/afinfo-song.wav.txt"'
check_cmd "afinfo: song.wav lasts the Song span ($SPAN s, within 0.01 s)" 'duration_close song.wav 0.01'

export_as m4a song.m4a
check_cmd "afinfo: song.m4a is an MPEG-4 audio file" 'grep -q "File type ID:   m4af" "$EVIDENCE/afinfo-song.m4a.txt"'
check_cmd "afinfo: song.m4a is 2 ch, $RATE Hz, AAC" 'grep -qE "2 ch, +$RATE Hz, aac" "$EVIDENCE/afinfo-song.m4a.txt"'
check_cmd "afinfo: song.m4a lasts the Song span ($SPAN s, within 0.05 s)" 'duration_close song.m4a 0.05'

snapshot final view activity history
journey_end
