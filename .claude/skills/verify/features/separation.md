# Separation

Separation splits one Backing Track into six new Backing Tracks: vocals, drums, bass, guitar, piano and other. Each goes on its own new Track below the source, and the source Track is muted. The app does the splitting in the `overdub-separate` helper process with the user-installed BS-RoFormer model.

## Sub-features

- `separation-helper`: the bundled helper writes six 44.1 kHz stereo float parts, each as long as the input, and reports progress on stdout as JSON lines.
- `separation-helper-errors`: a missing model, an unreadable input or an unwritable output gives one JSON error line on stderr and exit 1.
- `separation-in-song`: Separate... on a Track adds the six Tracks to the Song as one undo step, and the source Track is muted.
- `separation-cancel`: cancelling the modal progress dialog leaves the Song unchanged.

## How to get to it (user POV)

- Right-click a Record row whose Clips all play one Backing Track, then choose **Separate...**. The modal progress dialog shows until it finishes.
- Developers can also run `overdub-separate <model> <input> <out dir>` on its own (`docs/building.md`, "Separation helper").

## Driving it with separation-proof.sh

Preconditions:

- `launch.sh` and `doctor.sh` passed. The helper is the one bundled into this build's app.
- The model is installed at `~/Library/Application Support/Overdub/models/bs_roformer_sw.fp32.onnx` (by `make separation-model`).
- About 5 GB of free memory.

- **Run the helper.** Run `.claude/skills/verify/scripts/separation-proof.sh`. It exits 0 after about 55 s. `separation/stdout.jsonl` has `separation.started` with `"chunks":1` and `"samples":264600`, then `separation.chunk_done`, six `separation.part_written` lines and `separation.finished` (about 4.6 GB `peak_memory_mb`).
- **Parts are real.** Read `separation/parts-rms.txt`. There are six files, each `frames=264600`. `bass.wav` is around −13 dBFS and every other part is below −90 dBFS, because the input is a 110 Hz sine.
- **Error path.** Run the helper with a missing model: `build/debug/src/app/Overdub_artefacts/Debug/Overdub.app/Contents/MacOS/overdub-separate /nonexistent.onnx "$RUN_DIR/separation/input.wav" "$RUN_DIR/sep-err"; echo $?`. It exits `1`, and stderr has one line with `"error":"separation.model_missing"`.
- **In the Song (unverified).** `separation-in-song` and `separation-cancel` need the right-click menu and the modal dialog. Report them as unverified (no GUI driver). Once the UI has run, the proof is `song.separation_started` and `song.separation_finished` in the log, six new Backing Tracks with `separatedFrom` in `song.json`, and the source Track muted.

## Gotchas

- The model can never be redistributed or faked. Without it, report `separation-helper` as blocked by the missing precondition. Do not substitute `tests/separation/OnnxTestModel.h`.
- One run takes about 4.3× the input's length and uses about 900% CPU. Keep the synthetic input short.
- The Separate... item is disabled unless the Track's Clips all play one Backing Track. A Track with recorded Takes cannot be separated.
