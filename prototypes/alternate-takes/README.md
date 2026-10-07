# PROTOTYPE: Alternate takes (issue #15)

Throwaway. Never production; the real UI is JUCE Components (ADR 0001). Built on the agreed main window (issue #9, variant C).

Run: `open prototypes/alternate-takes/index.html`

Question: how do you browse the older Takes a newer Take covers, and bring one back? And should v1 have pre-roll and cycle recording?

Three variants of take browsing. Switch with ← → or the white pill (`?variant=A|B|C`). The pill's "state" button shows every Take with its status (heard / % heard / covered / no Clip) and every Clip in layer order.

- **A: Swap on the Clip.** A "Takes ▾ n" tag on any Take Clip with other Takes under it. Picking a Pass swaps that Clip's source Take in place: its position, trim, fades and gain stay, and it's trimmed to where the new Take has audio. Split first (S) to choose per section.
- **B: Bring to top.** Same tag. Picking a Pass lays a new Clip of that Take on top over the Clip's span. Nothing underneath changes, so Clips pile up (see the state panel).
- **C: Take lanes.** No tag. "▸ n Takes" in a Track header unfolds one thin lane per Take, newest first, with the heard parts filled in. Clicking a lane swaps the top Clip under that point to this Take (the same model as A). With no Clip there, the whole Take comes back on top.

In every variant:
- **Passes list** (Record mode's right column, and "Passes ▾" in Edit) shows each Pass's status. **↺ Bring back** re-lays the whole Pass on top on every Track it recorded, exactly as recorded, in one undo step. This is how you reach a Take no Clip uses (fixture: Pass 4, a vocal ad-lib at 1:40 whose Clip was deleted).
- **Pre-roll** (top bar: off / 1 / 2 / 4 s): R starts playback that many seconds before the record point. The Pass starts at the record point.
- **Cycle** (⟳ in the top bar, or C; drag the yellow bar on the ruler): playback loops the range. R records a new Pass on every loop, and the whole run is one undo step ("Record Passes 5–7"). A last loop under 0.5 s is dropped. Otherwise a partial last loop is kept.

Fixture: Guitar has Pass 1 (a full take, 0:20–1:20), and Passes 2 and 3 are two chorus retakes (0:38–1:02) on Guitar and Vocals.
