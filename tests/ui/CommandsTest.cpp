#include "ui/Commands.h"

#include <catch2/catch_test_macros.hpp>

using overdub::ui::Command;
using overdub::ui::commandForKey;
using overdub::ui::KeyChord;
using overdub::ui::Mode;
using overdub::ui::modeFor;

TEST_CASE ("keys 1 to 3 pick the Record, Edit and Export modes", "[ui][commands]")
{
    CHECK (commandForKey (KeyChord { '1' }) == Command::showRecord);
    CHECK (commandForKey (KeyChord { '2' }) == Command::showEdit);
    CHECK (commandForKey (KeyChord { '3' }) == Command::showExport);
    CHECK (commandForKey (KeyChord { '4' }) == std::nullopt);

    CHECK (modeFor (Command::showRecord) == Mode::record);
    CHECK (modeFor (Command::showEdit) == Mode::edit);
    CHECK (modeFor (Command::showExport) == Mode::exportSong);
    CHECK (modeFor (Command::record) == std::nullopt);
}

TEST_CASE ("R records, Space plays or stops, either letter case", "[ui][commands]")
{
    CHECK (commandForKey (KeyChord { 'r' }) == Command::record);
    CHECK (commandForKey (KeyChord { 'R' }) == Command::record);
    CHECK (commandForKey (KeyChord { ' ' }) == Command::playOrStop);
}

TEST_CASE ("command-Z undoes and shift-command-Z redoes; modified digits and letters do nothing", "[ui][commands]")
{
    CHECK (commandForKey (KeyChord { 'z', true, false }) == Command::undo);
    CHECK (commandForKey (KeyChord { 'Z', true, true }) == Command::redo);
    CHECK (commandForKey (KeyChord { 'z' }) == std::nullopt);
    CHECK (commandForKey (KeyChord { 'r', true, false }) == std::nullopt);
    CHECK (commandForKey (KeyChord { '1', true, false }) == std::nullopt);
    CHECK (commandForKey (KeyChord { ' ', false, true }) == std::nullopt);
    CHECK (commandForKey (KeyChord { 'R', false, true }) == std::nullopt);
}

TEST_CASE ("S splits a Clip and backspace or forward delete deletes one", "[ui][commands]")
{
    CHECK (commandForKey (KeyChord { 's' }) == Command::splitClip);
    CHECK (commandForKey (KeyChord { 'S' }) == Command::splitClip);
    CHECK (commandForKey (KeyChord { overdub::ui::backspaceCharacter }) == Command::deleteClip);
    CHECK (commandForKey (KeyChord { overdub::ui::forwardDeleteCharacter }) == Command::deleteClip);
    CHECK (commandForKey (KeyChord { 's', true, false }) == std::nullopt);
    CHECK (commandForKey (KeyChord { overdub::ui::backspaceCharacter, true, false }) == std::nullopt);
    CHECK (modeFor (Command::splitClip) == std::nullopt);
    CHECK (modeFor (Command::deleteClip) == std::nullopt);
}
