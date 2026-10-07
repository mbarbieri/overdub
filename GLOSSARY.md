# Overdub

A simple macOS app for recording songs one part at a time over imported backing tracks, with optional camera video captured alongside the audio.

## Language

### Song structure

**Song**:
The single document a user works in: its tracks, takes, mixer settings and video takes. Time is measured in seconds; there is no tempo or bar grid.
_Avoid_: Project, session, arrangement

**Track**:
A horizontal lane in a song that holds takes or a backing track, with its own mixer channel and insert chain.
_Avoid_: Channel (when you mean the lane), lane

**Take**:
One continuous recording pass on a track, from when recording starts to when it stops. It stores the dry input. Effects are never baked into it.
_Avoid_: Recording, pass

**Clip**:
A non-destructive window onto a take or backing track on the timeline, with its own trim, fades and gain.
_Avoid_: Region, segment

**Backing Track**:
An existing audio file imported onto its own track to play along with, typically an already recorded song. It is never generated or sequenced in the app.
_Avoid_: Loop, accompaniment, stem

**Video Take**:
Camera footage captured during the same pass as audio takes, which stays aligned with them on the timeline. A song has at most one camera armed at a time.
_Avoid_: Video track, camera clip

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
