# Transcribe mode listens through its own mix and per-Track aids; Loops carry their speed

Transcribe mode is for learning a part by ear, so what you do there to *hear* a part never changes what the Song *is*. That's the same split ADR 0009 makes between Practice Speed and Transpose. Transcribe mode has its own **Transcribe Mix** (mute and solo per Track) and one **Focus Track**, which picking solos. Each Track has its own octave shift (−1…+2) and band filter, and only the Focus Track's are heard. The Song's mix, its Insert Chains and its Export never see any of this. These listening settings are saved in the Song outside history, like Export's settings (issue #201), because you learn a song over several sittings. Loops and Markers, on the other hand, are part of the Song and take undo steps: they're shown in every mode and editable in Transcribe and Edit. Each **Loop** stores the Practice Speed it's drilled at. Activating a Loop applies that speed, and changing the speed while it's active updates the Loop. Leaving Transcribe mode returns Practice Speed to 100%, so Record never comes back refused because of something set while transcribing.

## Considered Options

- **Sharing the Song's mute and solo**: simpler, but muting the drums to hear the guitar would change your recording mix and your Export.
- **A Song-wide octave shift**: it would drag the drums and vocals up along with the separated bass you're trying to hear.
- **Octave shift and filter belonging to the Focus, not the Track**: switching Focus from bass to guitar would carry over a bass's settings that are wrong for a guitar.
- **One global Practice Speed, never saved (as in ADR 0009)**: every return to a Loop would mean re-dialling its speed.
- **Pitch analysis (spectrum, note and chord guessing, a reference keyboard)**: left out on purpose. Transcribe mode is listening tools only, and the player's instrument is the reference.

## Consequences

- **The octave shift is an offline render**, like Practice Speed: a temporary file outside the package, made from the Focus Track's sources in the same single pass as Transpose and Practice Speed (ADR 0009's "one render from the original"). Unlike a Practice Speed change, the new shift is heard at once: the Focus Track plays silent, with a spinner, until its render is there (issue #233). Its render is always of the whole Song, even while a Loop loops.
- **The band filter is real-time** on the Focus Track (two biquads, no render), so it follows its controls instantly.
- **Changing the Focus Track** re-solos the Transcribe Mix and may start an octave-shift render. Like any pending render, the source plays silent until it's ready.
- **Loops never play outside Transcribe mode.** In Record, clicking a Loop or a Marker only moves the record point. In Edit, it selects the Loop or Marker for editing.
- **Export can write a Loop's range** for every format, always the Song's own mix at 100%.
