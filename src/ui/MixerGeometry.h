#pragma once

#include <string>

// The Mixer's controls as pure mappings (issue #31, ADR 0005 UI amendment): fader position <-> dB,
// a fader or pan drag -> its new value, the meters' scale and fall, and the strips' texts.
// Positions are fractions of a control's travel: 0 = bottom (or hard left), 1 = top (hard right).
//
// The detent: a drag that reaches 0 dB (fader) or C (pan), or starts there, holds at exactly that
// value for detentPixels of further travel, then continues, so every value stays reachable. An
// arrow key press that would cross 0 dB or C stops there first.
//
// The fader law: the lower three quarters of the travel are -60 .. 0 dB, linear in dB; the top
// quarter is 0 .. +12 dB. Meters use the same scale, so 0 dBFS lines up with 0 dB on the fader.
namespace overdub::ui
{
// Where 0 dB sits on the fader's travel.
inline constexpr double faderUnityPosition = 0.75;

// The shown meter level is multiplied by this at each poll without a higher peak (about -1 dB per
// 30 ms poll).
inline constexpr float meterFallPerPoll = 0.89f;

// Position (clamped to [0, 1]) -> dB in [song::minVolumeDb, song::maxVolumeDb].
double faderPositionToDb (double position);
// dB (clamped to the fader's range) -> position in [0, 1].
double dbToFaderPosition (double db);

// The extra drag travel, in pixels, a fader or pan holds at 0 dB or C.
inline constexpr int detentPixels = 8;

// The dB a fader drag ends on: from `startDb`, moved by louderPixels / travelPixels of the travel
// (positive is louder, whatever the fader's orientation) less the detent if it reached 0 dB,
// clamped to the range, rounded to a tenth of a dB. No travel: startDb.
double faderDragDb (double startDb, int louderPixels, int travelPixels);

// The pan a drag ends on: from `startPan`, moved by 2 * deltaX / widthPixels (the width spans hard
// left to hard right) less the detent if it reached C, clamped to [-1, 1], rounded to hundredths.
// No width: startPan.
double panDrag (double startPan, int deltaX, int widthPixels);

// One arrow key press on a fader: 1 dB (0.1 when fine) louder or quieter, stopping at 0 dB if it
// would cross it, clamped to the range, rounded to a tenth.
double faderKeyStepDb (double db, bool louder, bool fine);

// One arrow key press on a pan control: 0.05 (0.01 when fine) right or left, stopping at C if it
// would cross it, clamped to [-1, 1], rounded to hundredths.
double panKeyStep (double pan, bool right, bool fine);

// Pan [-1, 1] -> position [0, 1] across the pan control.
double panPosition (double pan);

// A linear peak -> its position on the fader's scale; 0 for silence or anything below -60 dBFS,
// 1 at +12 dBFS and over.
double meterPosition (float peak);

// The level a meter shows after a poll that measured `peak`: the peak if higher, else the
// previous level fallen by meterFallPerPoll.
float nextMeterLevel (float shown, float peak);

// "C" within half a hundredth of centre, else "L25" / "R100" (percent of the way to that side).
std::string panText (double pan);

// "0.0 dB", "-6.3 dB", "+4.5 dB".
std::string volumeText (double db);
} // namespace overdub::ui
