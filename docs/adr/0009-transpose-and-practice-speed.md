# Transpose and Practice Speed are offline renders with Signalsmith Stretch; Transpose is lasting, Practice Speed is rehearsal only

A Song has one Transpose (whole semitones, −12…+12, default 0) applied to every Backing Track, and one Practice Speed (whole percent, 50…125, default 100) applied to everything heard, pitch unchanged. They are deliberately different kinds of thing. Transpose is part of the Song: it is an undo step, Takes are recorded in the key you hear, and Export plays it. Practice Speed is only for rehearsing: it is not saved (the Song always opens at 100%), Record is unavailable while it is not 100%, and Export ignores it. Both are rendered off the audio thread into cached files that playback swaps in when ready, using Signalsmith Stretch (MIT). This keeps ADR 0004's rule that the audio thread only reads float WAV at the Song's rate.

## Considered Options

- **Real-time stretching on the audio thread**: changes would apply instantly, but every Backing Track (and every Take at Practice Speed) would cost stretcher CPU and latency per block. ADR 0004 already rejected live conversion for the same reasons.
- **Rubber Band**: the best quality, but it is GPL or a paid commercial licence, which conflicts with shipping an MIT app.
- **SoundTouch** (LGPL): audibly worse on full mixes.
- **Practice Speed applied to Backing Tracks only**: Takes are placed in seconds, so they would play out of time with a slowed Backing Track. Slowing everything together keeps the Song in time. Letting a Pass record at a speed other than 100% would leave its Takes on a stretched timeline, so Record is unavailable instead.
- **Transpose as a setting outside history (like Practice Speed)**: rejected because Takes recorded after a Transpose change are in the new key. That change is part of the Song and must be undoable.

## Consequences

- **One render from the original.** Each render applies Transpose and Practice Speed together, in one pass from the Backing Track's derived `backing/<id>.wav` (or a Take's file), never by stretching an earlier render, so artefacts never compound.
- **Transpose renders** for the current value live in the package, and renders for other values are deleted. Undoing a Transpose change re-renders. For the semitone counts reached by undo, this keeps the package bounded instead of storing about 550 MB per value for six separated Backing Tracks.
- **Practice Speed renders** are temporary files outside the package. Only sources some Clip currently plays get rendered. A source with no render yet plays silent, and the Practice Speed field shows a spinner.
- **While a Transpose render is pending**, playback keeps the previous audio. Record and Export are unavailable ("Transposing…") through `ui/Availability.h`.
- **The timeline stays in Song time** at any Practice Speed: Clips, the ruler and loop points don't move, and the playhead moves slower. When a new Practice Speed's renders are ready, playback switches to them during play at the same Song time. The Video Preview pauses with a note while Practice Speed is not 100%.
- **Takes are never transposed.** Changing Transpose after Takes exist is allowed without confirmation, because undo is the safety net.
- **Separation always separates the original Backing Track.** The new Backing Tracks then get the Song's Transpose like any other.
- **Window 0.24 s, hop 15 ms** (`song/TransposeRender.cpp`), not Signalsmith's `presetDefault` (0.12 s, 30 ms), which put a sine up to 17 cents off its shifted pitch, past ADR 0005's 5. The cost is about 2.7 times the render time (about 35 s per 5-minute stereo Backing Track, one after another).
- **Signalsmith Stretch** is a submodule in `external/`. It links only into the code that renders, never into the audio thread's path, and it is listed in `THIRD_PARTY_NOTICES`.
