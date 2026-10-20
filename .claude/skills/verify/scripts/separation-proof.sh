#!/bin/zsh
# Runs the bundled overdub-separate on a synthetic 6 s input (110 Hz sine + clicks) the way the
# app does for a Separation, and records stdout, stderr, exit code and each part's RMS.
# Usage: .claude/skills/verify/scripts/separation-proof.sh [run-dir]   (default: build/verify/latest)
# Takes about 1 minute and about 4.6 GB of memory. Exit code is the helper's.
set -uo pipefail
REPO="$(cd "$(dirname "$0")/../../../.." && pwd)"
RUN_DIR="$(cd "${1:-$REPO/build/verify/latest}" && pwd -P)"
HELPER="$REPO/build/debug/src/app/Overdub_artefacts/Debug/Overdub.app/Contents/MacOS/overdub-separate"
MODEL="${OVERDUB_MODELS_DIR:-$HOME/Library/Application Support/Overdub/models}/bs_roformer_sw.fp32.onnx"
OUT="$RUN_DIR/separation"
mkdir -p "$OUT"

if [[ ! -f "$MODEL" ]]; then
    echo "separation-proof: no model at $MODEL; the user installs it with 'make separation-model' (Docker, ~1 GB)." >&2
    exit 1
fi

python3 -I - "$OUT/input.wav" <<'PY'
import math, struct, sys, wave
sr, n = 44100, 44100 * 6
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

echo "\$ overdub-separate <model> input.wav parts" >"$OUT/command.txt"
"$HELPER" "$MODEL" "$OUT/input.wav" "$OUT/parts" >"$OUT/stdout.jsonl" 2>"$OUT/stderr.txt"
code=$?
echo "$code" >"$OUT/exit-code"

python3 -I - "$OUT/parts" <<'PY' | tee "$OUT/parts-rms.txt"
import array, math, os, sys
d = sys.argv[1]
for name in sorted(os.listdir(d)) if os.path.isdir(d) else []:
    data = open(os.path.join(d, name), "rb").read()
    i = data.find(b"data")
    n = int.from_bytes(data[i + 4:i + 8], "little")
    a = array.array("f", data[i + 8:i + 8 + n])  # 32-bit float WAV; the wave module rejects format 3
    rms = math.sqrt(sum(x * x for x in a) / len(a)) if a else 0.0
    print(f"{name:12s} frames={len(a) // 2:7d} rms_dbfs={20 * math.log10(max(rms, 1e-12)):7.1f}")
PY
echo "exit=$code"
cat "$OUT/stderr.txt"
exit $code
