# PROTOTYPE: Transcribe mode page

Throwaway. Never production; the real UI is JUCE Components (ADR 0001).

Run: `open prototypes/transcribe-mode/index.html`

Question: what does the Transcribe mode page look like?

Four variants. Switch with ← → or the white pill (`?variant=A|B|C|D`). "keys" lists the shortcuts; "state" shows the full model and the Intent log.

- **A: Lanes.** Edit's timeline with every Track's Clip read-only. The lane headers carry the Transcribe Mix (◎ Focus, M, S). Listening controls, Loops and Markers in a side panel.
- **B: Focus wave.** One tall, zoomable waveform of the Focus Track (the whole Transcribe Mix when there is none). A compact Track list below with ◎/M/S. Same side panel.
- **C: Focus wave + overview.** B with a whole-Song overview on top: Loops, Markers and the visible window. Click or drag it to move.
- **D: Loop drill.** Loop cards on the left; the waveform zooms to the active Loop. Speed, Transpose, Focus aids and mixer strips in a bottom bar. No side panel.

Every variant follows the decisions settled so far: the Loop strip above the ruler (drag = new Loop, edge drag = resize, click = activate, double-click = rename), `[` `]` live Loops, `K` Markers, `L` looping, `⌥←` `⌥→` and `⌥1–9`, the per-Loop speed, 25–125 %, the Loop-first render, Transpose with cents, and an octave shift and band filter on the Focus Track. `H` for ½ speed is a placeholder key.

Fixture: a 3:30 Song with an Original mix Backing Track, its six Separated Backing Tracks and one guitar Take (Pass 1), two Loops and six Markers. It opens on "Solo bars 1–4" looping at 50 %.
