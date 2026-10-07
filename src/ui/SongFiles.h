#pragma once

#include "core/Error.h"
#include "song/SongPackage.h"

#include <juce_core/juce_core.h>

#include <memory>

// The File menu's Song operations behind the native dialogs (issue #27): what a chosen path
// means, and New Song, Open and Duplicate on it. MainWindow runs the dialogs; these run the rest,
// so the Notice and Alert paths are tested without a dialog.
namespace overdub::ui
{
// The Song rate when no device reports one (ADR 0004: the rate is fixed from the device at creation).
inline constexpr int fallbackSongSampleRate = 48000;

// A save dialog's choice as a package location: the path ending in .overdub.
juce::File packageLocationFor (const juce::File& chosen);

// An open dialog's choice as a package location: a file inside a package (song.json) means its folder.
juce::File packageLocationOpened (const juce::File& chosen);

// New Song: an empty Song at the device's rate (or fallbackSongSampleRate when it reports none)
// in a new package at the chosen location. Errors: SongPackage::create's (song.package_exists, ...).
core::Expected<std::unique_ptr<song::SongPackage>> createSongAt (const juce::File& chosen, int deviceSampleRate);

// Open: SongPackage::open on the chosen package. Its Notices are for the Notice line.
// Errors: SongPackage::open's (song.package_not_found, ...).
core::Expected<song::SongPackage::Opened> openSongAt (const juce::File& chosen);

// Duplicate: clones the package to the chosen location (SongPackage::duplicateTo) and returns
// that location. Errors: song.duplicate_failed, or the flush's.
core::Expected<juce::File> duplicateSongTo (song::SongPackage& package, const juce::File& chosen);
} // namespace overdub::ui
