# PROTOTYPE: Main window layout (issue #9)

Throwaway. Never production; the real UI is JUCE Components (ADR 0001).

Run: `open prototypes/main-window/index.html`

Three variants of the same window. Switch with ← → or the white pill (`?variant=A|B|C`). The pill's "state" button shows the in-memory Song model after every action.

- **A: Arrange + drawer.** Logic-style toolbar, inspector (selected Clip + Track strip + Master chain), timeline with a Camera lane on top, mixer drawer (X), floating Video Preview, Export sheet.
- **B: Wide rows.** No separate mixer: each Track row is its channel strip (vol, pan, Insert Chain chips). Master row at the bottom. Video Preview and Export docked in a right sidebar. Big transport at the bottom.
- **C: Modes (revised, default).** Record / Edit / Export (keys 1–3). Record: big camera stage, compact arm list with Insert Chains, Passes list, a Backing Track look-ahead strip (30 s window, playhead at 20%, "Silence in N s" / "Music back in N s"), whole-Song overview. Edit: timeline on top, mixer strips in the bottom half, floating Video Preview.

Keys: Space play/stop, R record (a Pass on all armed Tracks + camera), S split at playhead, ⌫ delete Clip, ⌘Z / ⇧⌘Z undo/redo, Enter to start.
Clips: drag to move, edges to trim, top-corner squares for fades, drag the dB tag up/down for gain. Hatching under a Clip marks an older Take it covers.
