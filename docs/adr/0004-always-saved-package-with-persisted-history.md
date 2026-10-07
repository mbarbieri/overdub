# A Song is an always-saved package with persisted snapshot history and content-addressed plugin state

A Song is a `.overdub` package folder: `song.json`, plus immutable media in `audio/`, `backing/` and `video/`, plugin state in `plugins/`, and undo history in `history/`. There is no Save command. You choose the package's location when you create the Song, recordings are written straight into it, and every change reaches `song.json` through a debounced atomic write (temp file + rename). Undo/redo is a stack of whole-Song snapshots of plain C++ value structs, not commands. The stack is persisted, so undo and redo survive closing and reopening the Song. We chose this because recordings already land on disk the moment they're made, so a separate "unsaved" state would only cover the JSON, and that's the confusing part. A snapshot can't drift out of sync with the model the way a hand-written inverse command can.

## Considered Options

- **Explicit Save, plus autosave to a side file offered for crash recovery**: familiar, but it means two write paths and a recovery dialog, and audio is already saved regardless.
- **Command-based undo (`juce::ValueTree` + `UndoManager`, or a hand-written command pattern)**: smaller history, but each inverse is code that can be wrong, and ValueTree's string-keyed properties and listeners work against flat, explicit code.
- **History stored as diffs (JSON Patch)**: smaller on disk, but undo becomes patch replay, which brings back the command-style fragility.
- **In-memory history only**: simpler, but the user wants undo to survive reopening.

## Consequences

- `history/NNNNNN.json` holds one full Song document plus a step label. `history/index.json` holds the current position. History is linear and capped at 200 steps. A new edit after an undo deletes the redo branch's files. A step costs about one document (~50 KB), so the full history is about 10 MB.
- Plugin state is content-addressed: `plugins/<sha256>.state`, written once and never modified. An Insert Chain slot stores the hash, so snapshots share blobs and an undo always restores the matching state. On open, blobs that neither `song.json` nor any history step references are deleted.
- Changes outside undo (parameters set in a plugin's own editor, arm, input assignment) still trigger the debounced write of `song.json` without adding a history step. Plugin state is re-captured on that write.
- Media files are immutable and Take files are never deleted implicitly, even when no Clip or snapshot references them.
- `formatVersion` migrations run one step function per version over `song.json` and every history snapshot. If any snapshot fails to migrate, or `history/` is missing or corrupt, the history is dropped (log entry plus a one-line notice). History never blocks opening a Song. Newer format versions are refused.
- Duplicating a Song `clonefile`s the whole package, history included.

## Amendment: Backing Track import and the peaks cache (issue #14)

A Backing Track is stored twice: the original file, kept untouched as `backing/<id>.orig.<ext>`, and a derived `backing/<id>.wav` (32-bit float at the Song's rate) that playback reads. The derived file is made at import even when the rates already match, so the audio thread only ever reads float WAV at the Song's rate, the same as a Take. The Song's rate stays fixed from the device at creation, and Backing Tracks are converted to it. Playback-time conversion was rejected: it needs a high-quality converter on the audio thread and makes seeking in MP3/AAC inexact.

- Decoding and conversion use Core Audio's `ExtAudioFile` at maximum converter quality. It trims AAC/MP3 encoder priming. Accepted formats are whatever it opens on macOS 15: WAV, AIFF/AIFC, CAF, M4A (AAC, ALAC), MP3, FLAC. Mono and stereo are kept as they are, and files with more than 2 channels are refused.
- `song.json` stores, per Backing Track, `id`, `originalFilename`, `originalExtension`, the original sample rate, channel count and format name, and `lengthSamples` at the Song's rate. There's no content hash and no de-duplication.
- Import writes `backing/<id>.wav.tmp` and renames it, then adds the Backing Track, a new Track named after the file and one Clip at sample 0 as a single undo step ("Import <name>"). One Track per file, so several Backing Tracks (for example per-instrument parts) line up at 0. On open, leftover `.tmp` files are deleted. A missing derived file is rebuilt from the original. If both are missing, the Clip plays silence and is shown as missing.
- `peaks/<media-id>.peaks` holds a min/max overview per 256 samples per channel for every Take and Backing Track. It is written at import, or when a Pass ends. It is a cache: it is never part of history, and it is rebuilt silently (logged) when missing or corrupt. Record mode's look-ahead strip shows the combined envelope of the currently audible Backing Tracks, computed from these files.

## Amendment: alternate takes and pre-roll (issue #15)

Older Takes are browsed in **Take Lanes**: a Track's header unfolds one thin lane per Take recorded on that Track, newest Pass first, with the spans where that Take is currently heard filled in. Clicking a lane makes the top Clip at that point play that Take. A per-Clip "Takes ▾" menu and "new Clip on top" were rejected: the menu hides the history behind a click, and stacking a new Clip per choice piles up layers and noisy covered-span hatching. Prototype on `prototype/alternate-takes`.

- A Take also stores `trackId`, the Track it was recorded on. A Track's lanes list exactly the Takes with its id. Takes whose Track has been deleted aren't shown, and Bring back skips them.
- **Swap**: clicking lane T at time t, where the top Clip at t plays a Take and T has audio at t, changes that Clip's source to T. It keeps its layer, gain and fades, and the source offset is recomputed so it stays aligned (timeline start − T's origin). The Clip is trimmed to where T has audio, so older layers show through at the edges, and the fades are clamped to the new length. One undo step: "Use Pass N on <Track>".
- If no Take Clip sounds at t (a gap, a Backing Track Clip, or a Take that no Clip uses), all of T comes back as a new Clip on the top layer, as recorded. One undo step.
- Picking a different Take per section means splitting the Clip first. Comping (swiping across lanes) stays out of scope.
- **Bring back a Pass**: each row of the Passes list (Record mode, and "Passes ▾" in Edit) shows whether the Pass is heard, partly heard or not heard, plus a **↺ Bring back** button. It adds one new top-layer Clip per Take of that Pass, on every Track it recorded, as recorded. One undo step: "Bring back Pass N". The Video Take is still picked separately at Export.
- Which lanes are unfolded is transient UI state, like selection.
- **Pre-roll**: off / 1 / 2 / 4 s, a Song setting that is persisted in `song.json` but isn't undoable. R starts playback that many seconds before the record point. The Pass, its Takes and its Video Take start at the record point, and only that span is written. The countdown shows in the transport clock and in Record mode's look-ahead strip.
- **Cycle recording** (one Pass per loop of a range) is out of scope for v1. It would split audio and camera capture at sample-exact loop boundaries mid-recording, which is new risk on the A/V sync path ADR 0003 proved.

## Amendment: history and package details settled by the `src/song` implementation (issue #23)

- **Snapshot files**: `history/NNNNNN.json` is `{ "label", "song" }`. Step numbers only grow (zero-padded to six digits, wider past 999999) and are never renumbered. The oldest snapshot is the baseline (the Song before the oldest undoable step): creating a Song, or dropping its history, writes a baseline with label `""`. With the 200-step cap there are at most 201 snapshot files, and pushing step 201 deletes the oldest file.
- **`history/index.json`** is `{ "oldest", "newest", "current" }`. Writes go snapshot file, then index, then deletions, all atomic, so a crash at any point still loads. Files outside `[oldest, newest]` are deleted on load.
- **Undo keeps state outside history**: undo and redo restore a snapshot but keep the current pre-roll, camera arm and device, playhead, export Video Take choice (unless the snapshot lacks that Video Take), and each Track's Armed and input channels (matched by Track id). Plugin parameters are outside history too: every Insert Chain slot that exists in both the current Song and the snapshot (matched by slot id, Master's chain included) keeps its current state hash. A slot that only the snapshot has, such as a plugin brought back by undoing its removal, gets the snapshot's hash. This narrows the Consequences bullet above: undo restores which plugins are in a chain, not their parameters.
- **Debounce**: the `song.json` write happens at most `saveDelay` (default 500 ms) after the first unsaved change. Later changes don't push the deadline back, so a stream of changes such as the playhead still gets written. History files and the index are written at once, not debounced. Closing the package flushes.
- **Migration**: `song.json` is written back at the current version on open. Snapshot files are never rewritten: each is migrated in memory on every load. A snapshot that fails to migrate (including one at a newer version) drops the history, but only a newer `song.json` refuses the open.
- **Dropped history** logs `song.history_dropped` (warn, with the cause's Error code) and returns a `Notice` in the open result, for the UI to show as its one-line notice.
- **Temp files**: every `*.tmp` under the package, at any depth, is deleted on open.
- **Plugin blobs**: the hash is the lowercase hex SHA-256 of the state bytes (CommonCrypto). Blobs are written once via temp + rename. On open, any blob that neither `song.json` nor any loaded snapshot references is deleted. A blob stored but not yet referenced by a saved `song.json` or a snapshot is therefore lost if the app quits first.
- **Create** refuses an existing location (`song.package_exists`) and makes `audio/`, `backing/`, `video/`, `plugins/` and `history/` up front.
