# Overdub

A simple macOS app for recording songs one part at a time over imported backing tracks, with optional camera video captured alongside the audio.

## Language

### Song structure

**Song**:
The single document a user works in: its tracks, takes, mixer settings and video takes. Time is measured in seconds; there is no tempo or bar grid.
_Avoid_: Project, session, arrangement

**Track**:
A horizontal lane in a song that holds clips of takes, backing tracks or both, with its own mixer channel and insert chain.
_Avoid_: Channel (when you mean the lane), lane

**Pass**:
One press of record through to stop. It produces one take on every armed track, plus a video take if the camera is armed.
_Avoid_: Session, run

**Take**:
The dry input one track captured during a pass. Effects are never baked into it.
_Avoid_: Recording

**Clip**:
A non-destructive window onto a take or backing track on the timeline, with its own trim, fades and gain. Clips on a track are stacked in layer order. Where they overlap, only the top one sounds.
_Avoid_: Region, segment

**Backing Track**:
An existing audio file imported onto its own track to play along with, typically an already recorded song. A song can have several, for example one per instrument when the original was delivered as separate parts, or when a Separation produced them. It is never generated or sequenced in the app.
_Avoid_: Loop, accompaniment, stem

**Separation**:
Splitting one Backing Track into new Backing Tracks, one per instrument (vocals, drums, bass, guitar, piano, other), each on its own new Track. The new Backing Tracks behave like any other; they only remember which Backing Track they came from.
_Avoid_: Stem split, demix, unmix

**Video Take**:
Camera footage captured during a pass, which stays aligned with that pass's takes on the timeline. A song has at most one camera armed at a time.
_Avoid_: Video track, camera clip

**Take Lane**:
A thin row under a Track, one per Take recorded on that Track, showing where that Take is currently heard. Clicking it makes the Clip at that point play that Take (a Swap), or brings the whole Take back on top where no Take Clip sounds.
_Avoid_: Comp lane, playlist, take folder

**Pre-roll**:
Playback that starts a few seconds before the record point so you can come in on time. The Pass still starts at the record point.
_Avoid_: Count-in (that implies a tempo)

### Signal flow

**Insert Chain**:
The ordered list of AU effect plugins that processes a track's (or the master's) audio.
_Avoid_: FX rack, plugin chain

**Master**:
The single output channel that every track sums into. It has its own insert chain.
_Avoid_: Main bus, stereo out

**Armed**:
A track (or the camera) that will capture input the next time recording starts.
_Avoid_: Enabled, selected

**Monitoring**:
Hearing an armed track's live input through its insert chain while playing.
_Avoid_: Input echo, thru
