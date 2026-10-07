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
