#include "control/AppState.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "ui/UiHarness.h"
#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cxxabi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <typeinfo>
#include <utility>
#include <vector>

// The UI → Intent proof (ADR 0008, issue #167): every gesture the window's Components offer is
// fired at a Song with Tracks, Clips, a Pass, a Video Take, an Insert and a selection, in each
// mode, and a gesture that changes the app state without a ui.intent_run log line in between is a
// bypass. notYetMigrated names the bypasses the pages still have (issue #168 moves them); it may
// only shrink: an entry that no longer bypasses fails too.
//
// Each mode gets a fresh CensusWorkspace, and before each probe its Song, selection and timeline
// view are put back as they were built, so no gesture changes what a later one is fired at.
//
// What it cannot see: a handler that runs some Intent and also edits the app state directly (the
// ui.intent_run line hides the bypass); text typed into an editor (Labels and TextEditors get
// clicks and keys, never typed text); and a change that lands after the gesture returns (an
// asynchronous callback, a timer, a dialog answered later).
//
// To regenerate notYetMigrated after a page migrates: run this test case with
// OVERDUB_CENSUS_PRINT_BYPASSES=1, which prints every bypass as an entry of the list.
using namespace overdub;
using namespace overdub::ui::intent;
using nlohmann::json;
using overdub::ui::testing::WorkspaceFixture;

namespace
{
// Paths of the app state that change without any gesture: the meters follow the engine.
const std::vector<std::string> movesOnItsOwn { "/meters" };

// "<mode>/<Component path>:<gesture>", each Component named by its class and its index among
// siblings of that class. Issue #168 migrates these pages and deletes their entries.
const std::set<std::string> notYetMigrated {
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#2:click",
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#2:right_click",
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#3:click",
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#3:right_click",
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#4:click",
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#4:right_click",
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#5:click",
    "edit/SongView#0/EditPage#0/ClipToolbar#0/TextButton#5:right_click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#0/InsertChainView#0/TextButton#0:click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#0/InsertChainView#0/TextButton#0:right_click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#1/InsertChainView#0/TextButton#0:click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#1/InsertChainView#0/TextButton#0:right_click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#2/InsertChainView#0/InsertChip#0/TextButton#0:click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#2/InsertChainView#0/InsertChip#0/TextButton#0:right_click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#2/InsertChainView#0/InsertChip#0/TextButton#1:click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#2/InsertChainView#0/InsertChip#0/TextButton#1:right_click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#2/InsertChainView#0/InsertChip#0:click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#2/InsertChainView#0/TextButton#0:click",
    "edit/SongView#0/EditPage#0/Mixer#0/MixerStrip#2/InsertChainView#0/TextButton#0:right_click",
    "edit/SongView#0/EditPage#0/MixerDivider#0:drag",
    "edit/SongView#0/EditPage#0/TextButton#0:click",
    "edit/SongView#0/EditPage#0/TextButton#0:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#0:click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#0:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#0:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#2:click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#2:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#2:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#3:click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#3:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TextButton#4:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#0/TrackGrip#0:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#0:click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#0:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#0:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#1:click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#1:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#1:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#2:click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#2:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#3:click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#3:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#3:right_click",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TextButton#4:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0/TrackHeader#1/TrackGrip#0:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0/Component#0:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0:click",
    "edit/SongView#0/EditPage#0/Timeline#0:double_click",
    "edit/SongView#0/EditPage#0/Timeline#0:drag",
    "edit/SongView#0/EditPage#0/Timeline#0:pinch",
    "edit/SongView#0/EditPage#0/Timeline#0:right_click",
    "export/SongView#0/ExportPage#0/TextButton#0:click",
    "export/SongView#0/ExportPage#0/TextButton#1:click",
    "export/SongView#0/ExportPage#0/ToggleButton#1:click",
    "export/SongView#0/ExportPage#0/ToggleButton#2:click",
    "record/SongView#0/RecordPage#0/ComboBox#0:choose:1",
    "record/SongView#0/RecordPage#0/ComboBox#0:choose:2",
    "record/SongView#0/RecordPage#0/ComboBox#0:choose:3",
    "record/SongView#0/RecordPage#0/ComboBox#1:choose:1",
    "record/SongView#0/RecordPage#0/PassesPanel#0/Row#0/TextButton#0:click",
    "record/SongView#0/RecordPage#0/PassesPanel#0/Row#0/TextButton#0:right_click",
    "record/SongView#0/RecordPage#0/SongOverview#0:click",
    "record/SongView#0/RecordPage#0/TextButton#0:click",
    "record/SongView#0/RecordPage#0/TextButton#0:right_click",
    "record/SongView#0/RecordPage#0/TextButton#2:click",
    "record/SongView#0/RecordPage#0/TextButton#2:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#0/ComboBox#0:choose:1",
    "record/SongView#0/RecordPage#0/TrackRow#0/ComboBox#0:choose:2",
    "record/SongView#0/RecordPage#0/TrackRow#0/ComboBox#0:choose:3",
    "record/SongView#0/RecordPage#0/TrackRow#0/InsertChainView#0/TextButton#0:click",
    "record/SongView#0/RecordPage#0/TrackRow#0/InsertChainView#0/TextButton#0:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#0:click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#0:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#1:click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#1:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#2:click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#2:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#3:click",
    "record/SongView#0/RecordPage#0/TrackRow#0/TextButton#3:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#1/ComboBox#0:choose:0",
    "record/SongView#0/RecordPage#0/TrackRow#1/ComboBox#0:choose:1",
    "record/SongView#0/RecordPage#0/TrackRow#1/ComboBox#0:choose:2",
    "record/SongView#0/RecordPage#0/TrackRow#1/ComboBox#0:choose:3",
    "record/SongView#0/RecordPage#0/TrackRow#1/InsertChainView#0/InsertChip#0/TextButton#0:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/InsertChainView#0/InsertChip#0/TextButton#0:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#1/InsertChainView#0/InsertChip#0/TextButton#1:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/InsertChainView#0/InsertChip#0/TextButton#1:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#1/InsertChainView#0/InsertChip#0:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/InsertChainView#0/TextButton#0:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/InsertChainView#0/TextButton#0:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#0:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#0:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#1:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#1:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#2:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#2:right_click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#3:click",
    "record/SongView#0/RecordPage#0/TrackRow#1/TextButton#3:right_click",
};

struct ComponentPath
{
    std::vector<int> childIndices;
    std::string id;

    // The window's content itself (the MainComponent), which key presses go to.
    static ComponentPath windowContent (std::string id = {}) { return { {}, std::move (id) }; }
};

struct Probe
{
    std::string id;
    ComponentPath path;
    std::function<void (juce::Component&)> fire;
};

std::string className (const juce::Component& component)
{
    int status = 0;
    const auto* mangled = typeid (component).name();
    std::unique_ptr<char, decltype (&std::free)> demangled { abi::__cxa_demangle (mangled, nullptr, nullptr, &status), &std::free };
    std::string name = status == 0 ? demangled.get() : mangled;
    if (const auto colons = name.rfind ("::"); colons != std::string::npos)
        name = name.substr (colons + 2);
    return name;
}

juce::Component* resolve (juce::Component& root, const ComponentPath& path)
{
    auto* component = &root;
    for (const auto index : path.childIndices)
    {
        if (index >= component->getNumChildComponents())
            return nullptr;
        component = component->getChildComponent (index);
    }
    return component;
}

void mouseAt (juce::Component& target, juce::ModifierKeys buttons)
{
    const auto centre = target.getLocalBounds().getCentre().toFloat();
    target.mouseDown (ui::testing::mouseEventAt (target, centre, buttons));
    target.mouseUp (ui::testing::mouseEventAt (target, centre, juce::ModifierKeys()));
}

json::array_t changedPaths (const json& before, const json& after)
{
    json::array_t changed;
    for (const auto& operation : json::diff (before, after))
    {
        const auto path = operation.at ("path").get<std::string>();
        const auto moves = std::ranges::any_of (movesOnItsOwn, [&path] (const std::string& prefix) { return path.starts_with (prefix); });
        if (! moves)
            changed.push_back (path);
    }
    return changed;
}

std::vector<std::pair<std::string, std::function<void (juce::Component&)>>> gesturesFor (juce::Component& component, const juce::File& dropFile)
{
    std::vector<std::pair<std::string, std::function<void (juce::Component&)>>> gestures;
    if (auto* comboBox = dynamic_cast<juce::ComboBox*> (&component))
    {
        // A click opens a native pop-up: its items are picked instead, as a user's pick would.
        for (int i = 0; i < comboBox->getNumItems(); ++i)
            gestures.emplace_back ("choose:" + std::to_string (i), [i] (juce::Component& c) {
                auto& box = dynamic_cast<juce::ComboBox&> (c);
                if (i < box.getNumItems())
                    box.setSelectedItemIndex (i, juce::sendNotificationSync);
            });
        return gestures;
    }
    gestures.emplace_back ("click", [] (juce::Component& c) { mouseAt (c, juce::ModifierKeys (juce::ModifierKeys::leftButtonModifier)); });
    gestures.emplace_back ("right_click", [] (juce::Component& c) { mouseAt (c, juce::ModifierKeys (juce::ModifierKeys::rightButtonModifier)); });
    gestures.emplace_back ("double_click", [] (juce::Component& c) { ui::testing::doubleClickAt (c, c.getLocalBounds().getCentre()); });
    gestures.emplace_back ("drag", [] (juce::Component& c) {
        const auto centre = c.getLocalBounds().getCentre();
        ui::testing::drag (c, centre, centre + juce::Point<int> (12, 12));
    });
    gestures.emplace_back ("wheel", [] (juce::Component& c) {
        juce::MouseWheelDetails wheel { 0.0f, 0.2f, false, false, false };
        c.mouseWheelMove (ui::testing::mouseEventAt (c, c.getLocalBounds().getCentre().toFloat(), {}), wheel);
    });
    gestures.emplace_back ("wheel_sideways", [] (juce::Component& c) {
        juce::MouseWheelDetails wheel { 0.2f, 0.0f, false, false, false };
        c.mouseWheelMove (ui::testing::mouseEventAt (c, c.getLocalBounds().getCentre().toFloat(), {}), wheel);
    });
    gestures.emplace_back ("pinch", [] (juce::Component& c) { c.mouseMagnify (ui::testing::mouseEventAt (c, c.getLocalBounds().getCentre().toFloat(), {}), 1.5f); });
    if (dynamic_cast<juce::FileDragAndDropTarget*> (&component) != nullptr)
        gestures.emplace_back ("drop", [dropFile] (juce::Component& c) {
            auto& target = dynamic_cast<juce::FileDragAndDropTarget&> (c);
            const juce::StringArray files { dropFile.getFullPathName() };
            const auto centre = c.getLocalBounds().getCentre();
            if (! target.isInterestedInFileDrag (files))
                return;
            target.fileDragEnter (files, centre.x, centre.y);
            target.filesDropped (files, centre.x, centre.y);
        });
    return gestures;
}

void collectProbes (juce::Component& component, ComponentPath path, const juce::File& dropFile, std::vector<Probe>& probes)
{
    for (auto& [gesture, fire] : gesturesFor (component, dropFile))
        probes.push_back ({ path.id + ":" + gesture, path, fire });
    if (dynamic_cast<juce::ComboBox*> (&component) != nullptr)
        return;
    std::map<std::string, int> seen;
    for (int i = 0; i < component.getNumChildComponents(); ++i)
    {
        auto& child = *component.getChildComponent (i);
        const auto name = className (child);
        const auto ordinal = seen[name]++;
        if (! ui::testing::isShown (child) || ! child.isEnabled())
            continue;
        auto childPath = path;
        childPath.childIndices.push_back (i);
        childPath.id += "/" + name + "#" + std::to_string (ordinal);
        collectProbes (child, std::move (childPath), dropFile, probes);
    }
}

std::vector<Probe> keyProbes()
{
    std::vector<Probe> probes;
    const auto addKey = [&probes] (const std::string& name, juce::KeyPress key) {
        probes.push_back ({ "key:" + name, ComponentPath::windowContent(), [key] (juce::Component& window) { window.keyPressed (key); } });
    };
    for (const auto character : std::string ("abcdefghijklmnopqrstuvwxyz0123456789 ,.-=[]"))
        for (const auto command : { false, true })
            for (const auto shift : { false, true })
            {
                const auto modifiers = juce::ModifierKeys ((command ? juce::ModifierKeys::commandModifier : 0) | (shift ? juce::ModifierKeys::shiftModifier : 0));
                addKey (std::string (command ? "cmd+" : "") + (shift ? "shift+" : "") + std::string (1, character), juce::KeyPress ((int) character, modifiers, (juce::juce_wchar) character));
            }
    addKey ("left", juce::KeyPress (juce::KeyPress::leftKey));
    addKey ("right", juce::KeyPress (juce::KeyPress::rightKey));
    addKey ("up", juce::KeyPress (juce::KeyPress::upKey));
    addKey ("down", juce::KeyPress (juce::KeyPress::downKey));
    addKey ("shift+up", juce::KeyPress (juce::KeyPress::upKey, juce::ModifierKeys::shiftModifier, 0));
    addKey ("backspace", juce::KeyPress (juce::KeyPress::backspaceKey));
    addKey ("delete", juce::KeyPress (juce::KeyPress::deleteKey));
    addKey ("escape", juce::KeyPress (juce::KeyPress::escapeKey));
    addKey ("return", juce::KeyPress (juce::KeyPress::returnKey));
    addKey ("home", juce::KeyPress (juce::KeyPress::homeKey));
    return probes;
}

// The census's Workspace: a Song with a Backing Track, an armed Guitar, a recorded Pass with its
// Take and Video Take, an Insert on the Guitar and the Band Mix Clip selected. Every dialog is
// answered at once: an import picks a generated WAV, a Track delete is cancelled, "+ Insert" picks
// the one AU on offer, a Track menu is dismissed, an export goes to a temp file, camera access is
// granted. restore() puts back the Song (through its history), the selection and the view as built.
struct CensusWorkspace
{
    WorkspaceFixture f;
    juce::File dropFile;
    song::Id selectedClip;
    song::Song builtSong;
    json builtView;

    CensusWorkspace()
    {
        f.launch();
        f.openSong();
        dropFile = f.folder.getChildFile ("Drop.wav");
        writeSine (dropFile);
        const plugin::AvailableAudioUnit delay { { "aufx", "dely", "appl" }, "AUDelay", "Apple", "1.0" };
        f.content.listInsertPlugins = [delay] { return std::vector<plugin::AvailableAudioUnit> { delay }; };
        f.content.chooseInsertPlugin = [] (const auto&, std::function<void (std::optional<std::size_t>)> done) { done (0); };
        f.content.chooseFilesToImport = [this] (std::function<void (std::vector<juce::File>)> done) { done ({ dropFile }); };
        f.content.confirmTrackDelete = [] (const juce::String&, std::function<void (bool)> done) { done (false); };
        f.content.chooseTrackMenuItem = [] (const auto&, std::function<void (std::optional<std::size_t>)> done) { done (std::nullopt); };
        f.content.chooseExportDestination = [this] (juce::File, std::function<void (std::optional<juce::File>)> done) { done (f.folder.getChildFile ("Chosen.wav")); };
        f.content.revealInFinder = [] (const juce::File&) {};

        run (ui::SongIntent { SetCameraArmed { true } });
        run (ui::SongIntent { Record {} });
        f.runBlocks (40);
        run (ui::SongIntent { PlayOrStop {} });
        f.runBlocks (2);
        REQUIRE (f.song().song().passes.size() == 1);
        REQUIRE (f.song().song().videoTakes.size() == 1);
        const auto& song = f.song().song();
        const auto guitarId = song.tracks.at (1).id;
        run (ui::SongIntent { AddInsert { guitarId, "aufx", "dely", "appl" } });
        run (ui::SongIntent { SetTakeLanesUnfolded { guitarId, true } });
        selectedClip = f.song().song().tracks.front().clips.front().id;
        run (ui::SongIntent { SelectClip { selectedClip } });
        run (ui::SongIntent { Seek { 0 } });
        builtSong = f.song().song();
        builtView = control::appState (f.content, *f.workspace, std::array<std::string_view, 1> { "view" })["view"];
    }

    // A Pass a gesture started is stopped, so teardown never abandons one.
    ~CensusWorkspace() { stopTransport(); }

    void run (const ui::Intent& intent) { REQUIRE (f.content.run (intent, ui::Origin::control).has_value()); }

    void writeSine (const juce::File& file)
    {
        juce::AudioBuffer<float> audio (1, engine::testing::testSampleRate / 4);
        for (int i = 0; i < audio.getNumSamples(); ++i)
            audio.setSample (0, i, 0.25f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 440.0 * i / engine::testing::testSampleRate));
        REQUIRE (song::writeFloatWavAtomically (file, audio, engine::testing::testSampleRate, {}).has_value());
    }

    void stopTransport()
    {
        if (f.song().isRecording() || f.song().isPlaying())
        {
            f.content.run (ui::SongIntent { PlayOrStop {} });
            f.runBlocks (2);
        }
    }

    // The undoable content as built; the playhead, pre-roll and the rest outside history may differ.
    bool songAsBuilt()
    {
        const auto& current = f.song().song();
        return song::keepStateOutsideHistory (builtSong, current) == current;
    }

    // Undoes what gestures added to the history, or redoes what they undid, until the Song is as
    // built; then puts back each part of the view that differs.
    void restore()
    {
        while (! songAsBuilt() && f.song().package().canUndo())
            f.content.run (ui::SongIntent { Undo {} });
        while (! songAsBuilt() && f.song().package().canRedo())
            f.content.run (ui::SongIntent { Redo {} });
        // A gesture that edited outside history leaves no undo step to reach the built Song by.
        REQUIRE (songAsBuilt());
        const auto view = control::appState (f.content, *f.workspace, std::array<std::string_view, 1> { "view" })["view"];
        const auto differs = [&view, this] (const json::json_pointer& pointer) { return view.value (pointer, json()) != builtView.value (pointer, json()); };
        if (differs ("/selected_clip"_json_pointer) || differs ("/selected_video_take"_json_pointer))
        {
            f.content.run (ui::SongIntent { SelectVideoTake { std::nullopt } });
            f.content.run (ui::SongIntent { SelectClip { selectedClip } });
        }
        const auto& timeline = builtView["timeline"];
        if (differs ("/timeline/first_sample"_json_pointer) || differs ("/timeline/samples_per_pixel"_json_pointer))
            f.content.run (ui::SongIntent { SetTimelineView { timeline["first_sample"].get<song::SampleCount>(), timeline["samples_per_pixel"].get<song::SampleCount>() } });
        if (differs ("/timeline/take_lanes_unfolded"_json_pointer))
            for (const auto& track : f.song().song().tracks)
                f.content.run (ui::SongIntent { SetTakeLanesUnfolded { track.id, std::ranges::contains (timeline["take_lanes_unfolded"], json (track.id)) } });
        if (differs ("/timeline/vertical_scroll"_json_pointer))
            f.content.run (ui::SongIntent { SetVerticalScroll { timeline["vertical_scroll"].get<int>() } });
        if (differs ("/mixer_visible"_json_pointer))
            f.content.run (ui::SongIntent { SetMixerVisible { builtView["mixer_visible"].get<bool>() } });
        if (differs ("/mixer_height"_json_pointer))
            f.content.run (ui::SongIntent { SetMixerHeight { builtView["mixer_height"].get<int>() } });
        if (differs ("/passes_panel_open"_json_pointer))
            f.content.run (ui::SongIntent { SetPassesPanelOpen { builtView["passes_panel_open"].get<bool>() } });
    }

    void settle (ui::Mode mode)
    {
        if (auto* view = f.content.songView())
            view->commitKeyBursts();
        stopTransport();
        restore();
        f.content.run (ui::WindowIntent { ShowMode { mode } });
    }
};

struct CensusResult
{
    std::set<std::string> bypasses;
    std::map<std::string, std::string> changedBy;
    int fired = 0;
};

void fireProbe (CensusWorkspace& census, const Probe& probe, ui::Mode mode, CensusResult& result)
{
    census.settle (mode);
    auto* target = resolve (census.f.content, probe.path);
    if (target == nullptr || ! ui::testing::isShown (*target) || ! target->isEnabled())
        return;
    const auto before = control::appState (census.f.content, *census.f.workspace);
    core::ScopedLogCapture capture;
    probe.fire (*target);
    const auto after = control::appState (census.f.content, *census.f.workspace);
    ++result.fired;
    const auto ranIntent = std::ranges::any_of (capture.events(), [] (const json& event) { return event.at ("event") == "ui.intent_run"; });
    const auto changed = changedPaths (before, after);
    if (! changed.empty() && ! ranIntent)
    {
        result.bypasses.insert (probe.id);
        result.changedBy[probe.id] = json (changed).dump();
    }
}

CensusResult runCensus()
{
    CensusResult result;
    for (const auto mode : { ui::Mode::record, ui::Mode::edit, ui::Mode::exportSong })
    {
        CensusWorkspace census;
        census.settle (mode);
        const auto modeName = std::string (ui::modeName (mode));
        std::vector<Probe> probes;
        collectProbes (census.f.content, ComponentPath::windowContent (modeName), census.dropFile, probes);
        for (auto key : keyProbes())
        {
            key.id = modeName + "/" + key.id;
            probes.push_back (std::move (key));
        }
        for (const auto& probe : probes)
            fireProbe (census, probe, mode, result);
    }
    return result;
}
} // namespace

TEST_CASE ("no gesture changes the app state without running an Intent except the pages not migrated yet", "[control][census]")
{
    const auto result = runCensus();
    if (std::getenv ("OVERDUB_CENSUS_PRINT_BYPASSES") != nullptr)
        for (const auto& bypass : result.bypasses)
            std::printf ("    \"%s\",\n", bypass.c_str());

    for (const auto& [probe, changed] : result.changedBy)
    {
        INFO (probe << " changed " << changed << " without ui.intent_run");
        CHECK (notYetMigrated.contains (probe));
    }
    for (const auto& allowed : notYetMigrated)
    {
        INFO (allowed << " runs an Intent now, or no longer exists: remove it from the allow-list");
        CHECK (result.bypasses.contains (allowed));
    }
    CHECK (result.fired > 1000);
}

TEST_CASE ("the census fails on a button that edits the Song without an Intent", "[control][census]")
{
    CensusWorkspace census;
    juce::TextButton planted { "Planted" };
    planted.onClick = [&census] {
        auto changed = census.f.song().song();
        changed.master.volumeDb = -6.0;
        REQUIRE (census.f.song().edit (std::move (changed), "Planted").has_value());
    };
    census.f.content.addAndMakeVisible (planted);
    planted.setBounds (0, 0, 40, 20);

    std::vector<Probe> probes;
    collectProbes (census.f.content, ComponentPath::windowContent ("record"), census.dropFile, probes);
    const auto plantedProbe = std::ranges::find_if (probes, [] (const Probe& probe) { return probe.id == "record/TextButton#0:click"; });
    REQUIRE (plantedProbe != probes.end());
    CensusResult result;
    fireProbe (census, *plantedProbe, ui::Mode::record, result);

    CHECK (result.bypasses == std::set<std::string> { "record/TextButton#0:click" });
    CHECK (result.changedBy["record/TextButton#0:click"].find ("/song/song/master/volumeDb") != std::string::npos);
}
