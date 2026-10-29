#include "control/AppState.h"

#include "core/Log.h"
#include "song/AudioFileDecode.h"
#include "ui/SongMenuItems.h"
#include "ui/UiHarness.h"
#include "ui/WorkspaceFixture.h"

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <cxxabi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <utility>
#include <vector>

// The UI → Intent proof (ADR 0008, issue #167): every gesture the window's Components offer, and
// each File menu Song item (ui/SongMenuItems.h), is fired at a Song with Tracks, Clips, a Pass, a
// Video Take, an Insert and a selection, in each mode (Transcribe too, issue #225), with the Export sheet open (issue #200) and
// while an export runs (issue #202), and a gesture that changes the app state without running an Intent through
// MainComponent::run (its intentsRun count, which includes the per-frame Intents that log no
// ui.intent_run) is a bypass, and the census fails on any bypass.
//
// Each scene gets a fresh CensusWorkspace, and before each probe its Song, selection and timeline
// view are put back as they were built, an export a gesture started is finished, and the mode
// and the Export sheet and a running export are as the scene has them, so no gesture changes what
// a later one is fired at.
//
// What it cannot see: a handler that runs some Intent and also edits the app state directly (the
// Intent run hides the bypass); text typed into an editor (Labels and TextEditors get
// clicks and keys, never typed text); and a change that lands after the gesture returns (an
// asynchronous callback, a timer, a dialog answered later).

using namespace overdub;
using namespace overdub::ui::intent;
using nlohmann::json;
using overdub::ui::testing::WorkspaceFixture;

namespace
{
// Paths of the app state that change without any gesture: the meters follow the engine.
const std::vector<std::string> movesOnItsOwn { "/meters" };

struct ComponentPath
{
    std::vector<int> childIndices;
    std::string id;

    // The window's content itself (the MainComponent), which key presses go to.
    static ComponentPath windowContent (std::string id = {}) { return { {}, std::move (id) }; }
};

// What the window shows while a scene's probes fire: a mode, the Export sheet over it or not, and
// an export running (waiting for the audio, which no probe runs) or not.
struct Scene
{
    std::string name;
    ui::Mode mode = ui::Mode::record;
    bool exportSheet = false;
    bool exporting = false;
};

struct Probe
{
    // "<scene>/<Component path>:<gesture>", each Component named by its class and its index among
    // siblings of that class.
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

// Each File menu Song item, picked as MainWindow picks it: only while it is enabled.
std::vector<Probe> menuProbes()
{
    std::vector<Probe> probes;
    for (const auto& item : ui::songMenuItems)
        probes.push_back ({ "menu:" + std::string (item.name), ComponentPath::windowContent(), [&item] (juce::Component& window) {
            auto& content = dynamic_cast<ui::MainComponent&> (window);
            if (! content.refusalOf (ui::opName (ui::Intent { item.intent })).has_value())
                content.run (item.intent);
        } });
    return probes;
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
    // Loops (issue #228): option-left, option-right and option-1..9 select a Loop.
    const juce::ModifierKeys option { juce::ModifierKeys::altModifier };
    addKey ("alt+left", juce::KeyPress (juce::KeyPress::leftKey, option, 0));
    addKey ("alt+right", juce::KeyPress (juce::KeyPress::rightKey, option, 0));
    for (const auto character : std::string ("123456789"))
        addKey (std::string ("alt+") + character, juce::KeyPress ((int) character, option, 0));
    // Record mode's double-Space (issue #216): two Spaces at once are within any double-click interval.
    probes.push_back ({ "key:space,space", ComponentPath::windowContent(), [] (juce::Component& window) {
        window.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
        window.keyPressed (juce::KeyPress (juce::KeyPress::spaceKey));
    } });
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
        // Two Markers (issue #227), added where Markers are edited.
        run (ui::WindowIntent { ShowMode { ui::Mode::edit } });
        run (ui::SongIntent { Seek { 1000 } });
        run (ui::SongIntent { AddMarker {} });
        run (ui::SongIntent { Seek { 30000 } });
        run (ui::SongIntent { AddMarker {} });
        // Two Loops (issue #228); the second, made last, is active and looping.
        run (ui::SongIntent { AddLoop { 2000, 8000 } });
        run (ui::SongIntent { AddLoop { 20000, 26000 } });
        run (ui::WindowIntent { ShowMode { ui::Mode::record } });
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

    // An export a gesture started runs to its end: the Export sheet opens only when none runs.
    void finishExport()
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
        while (f.song().isExporting() && std::chrono::steady_clock::now() < deadline)
        {
            f.runBlocks (1);
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }
        REQUIRE (! f.song().isExporting());
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
        // select_clip also clears the selected Marker and Loop.
        if (differs ("/selected_clip"_json_pointer) || differs ("/selected_video_take"_json_pointer) || differs ("/selected_marker"_json_pointer)
            || differs ("/selected_loop"_json_pointer))
        {
            f.content.run (ui::SongIntent { SelectVideoTake { std::nullopt } });
            f.content.run (ui::SongIntent { SelectClip { selectedClip } });
        }
        const auto& timeline = builtView["timeline"];
        if (differs ("/timeline/first_sample"_json_pointer) || differs ("/timeline/samples_per_pixel"_json_pointer))
            f.content.run (ui::SongIntent { SetTimelineView { timeline["first_sample"].get<song::SampleCount>(), timeline["samples_per_pixel"].get<song::SampleCount>() } });
        if (differs ("/transcribe"_json_pointer))
            f.content.run (ui::SongIntent { SetTranscribeView { builtView["transcribe"]["first_sample"].get<song::SampleCount>(), builtView["transcribe"]["samples_per_pixel"].get<song::SampleCount>() } });
        if (differs ("/loop_start_pending"_json_pointer))
            f.content.run (ui::SongIntent { CancelLoopStart {} });
        if (differs ("/transcribe_column_visible"_json_pointer))
            f.content.run (ui::WindowIntent { SetTranscribeColumnVisible { builtView["transcribe_column_visible"].get<bool>() } });
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
        restoreTranscribeMix();
    }

    // The Focus Track, the Transcribe Mix and the octave shifts are outside history: ◎, M, S and
    // the octave buttons leave no undo step.
    void restoreTranscribeMix()
    {
        // The active Loop and looping (issue #228) are outside history too: put back directly,
        // since select_loop also moves the playhead and the view.
        if (f.song().song().activeLoopId != builtSong.activeLoopId || f.song().song().looping != builtSong.looping)
            REQUIRE (f.song().setActiveLoop (builtSong.activeLoopId, builtSong.looping).has_value());
        if (f.song().song().focusTrackId != builtSong.focusTrackId)
            f.content.run (ui::SongIntent { SetFocusTrack { builtSong.focusTrackId } });
        for (const auto& built : builtSong.tracks)
        {
            const auto* current = song::findTrack (f.song().song(), built.id);
            if (current == nullptr)
                continue;
            if (current->transcribeMute != built.transcribeMute)
                f.content.run (ui::SongIntent { SetTranscribeMute { built.id, built.transcribeMute } });
            if (current->transcribeSolo != built.transcribeSolo)
                f.content.run (ui::SongIntent { SetTranscribeSolo { built.id, built.transcribeSolo } });
            if (current->octaveShift != built.octaveShift)
                f.content.run (ui::SongIntent { SetOctaveShift { built.id, built.octaveShift } });
        }
    }

    void settle (const Scene& scene)
    {
        if (auto* view = f.content.songView())
            view->commitKeyBursts();
        stopTransport();
        if (scene.exporting && f.song().isExporting())
            run (ui::SongIntent { CancelExport {} });
        finishExport();
        restore();
        f.content.run (ui::SongIntent { ShowExport { false } });
        f.content.run (ui::WindowIntent { ShowMode { scene.mode } });
        if (scene.exportSheet)
            run (ui::SongIntent { ShowExport { true } });
        if (scene.exporting)
            run (ui::SongIntent { Export {} });
    }
};

struct CensusResult
{
    std::set<std::string> bypasses;
    std::map<std::string, std::string> changedBy;
    std::set<std::string> firedIds;
    int fired = 0;
};

void fireProbe (CensusWorkspace& census, const Probe& probe, const Scene& scene, CensusResult& result)
{
    census.settle (scene);
    auto* target = resolve (census.f.content, probe.path);
    if (target == nullptr || ! ui::testing::isShown (*target) || ! target->isEnabled())
        return;
    const auto before = control::appState (census.f.content, *census.f.workspace);
    const auto intentsBefore = census.f.content.intentsRun();
    const core::ScopedLogCapture quiet;
    probe.fire (*target);
    const auto after = control::appState (census.f.content, *census.f.workspace);
    ++result.fired;
    result.firedIds.insert (probe.id);
    const auto ranIntent = census.f.content.intentsRun() != intentsBefore;
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
    const Scene scenes[] {
        { "record", ui::Mode::record, false },
        { "edit", ui::Mode::edit, false },
        { "transcribe", ui::Mode::transcribe, false },
        { "export_sheet", ui::Mode::record, true },
        { "exporting", ui::Mode::record, false, true },
    };
    for (const auto& scene : scenes)
    {
        CensusWorkspace census;
        census.settle (scene);
        std::vector<Probe> probes;
        collectProbes (census.f.content, ComponentPath::windowContent (scene.name), census.dropFile, probes);
        for (auto key : keyProbes())
        {
            key.id = scene.name + "/" + key.id;
            probes.push_back (std::move (key));
        }
        for (const auto& probe : probes)
            fireProbe (census, probe, scene, result);
        for (const auto& probe : menuProbes())
            fireProbe (census, { scene.name + "/" + probe.id, probe.path, probe.fire }, scene, result);
    }
    return result;
}
} // namespace

TEST_CASE ("no gesture changes the app state without running an Intent", "[control][census]")
{
    // Kept for the TopBar and TranscribeColumn built below, after every census Workspace is gone.
    const juce::ScopedJuceInitialiser_GUI gui;
    const auto result = runCensus();
    for (const auto& [probe, changed] : result.changedBy)
        FAIL_CHECK (probe << " changed " << changed << " without running an Intent");
    CHECK (result.bypasses.empty());
    CHECK (result.fired > 1000);
    // The Camera lane header's A (issue #198), the Timeline's only direct TextButton, is fired.
    const auto cameraArm = std::ranges::find_if (result.firedIds, [] (const std::string& id) { return id.starts_with ("edit/") && id.ends_with ("/Timeline#0/TextButton#0:click"); });
    CHECK (cameraArm != result.firedIds.end());
    // The top bar's Export... and the Export sheet's Cancel and Export (issue #200) are fired.
    const auto fired = [&result] (std::string_view prefix, std::string_view suffix) {
        return std::ranges::any_of (result.firedIds, [&] (const std::string& id) { return id.starts_with (prefix) && id.ends_with (suffix); });
    };
    CHECK (fired ("record/TopBar#0/", ":click"));
    CHECK (fired ("export_sheet/ExportSheet#0/TextButton#", ":click"));
    // The top bar's ✕ and File > Cancel Export while an export runs (issue #202).
    const ui::TopBar bar;
    int cancelOrdinal = 0;
    for (int i = 0; bar.getChildComponent (i) != &bar.cancelExportButton; ++i)
        if (dynamic_cast<const juce::TextButton*> (bar.getChildComponent (i)) != nullptr)
            ++cancelOrdinal;
    CHECK (fired ("exporting/TopBar#0/TextButton#" + std::to_string (cancelOrdinal), ":click"));
    CHECK (fired ("exporting/menu:Cancel Export", ""));
    // The top bar's Practice Speed field (issue #212) is clicked and double-clicked (its editor).
    int practiceSpeedOrdinal = 0;
    for (int i = 0; bar.getChildComponent (i) != &bar.practiceSpeedField; ++i)
        if (dynamic_cast<const juce::Label*> (bar.getChildComponent (i)) != nullptr)
            ++practiceSpeedOrdinal;
    CHECK (fired ("record/TopBar#0/Label#" + std::to_string (practiceSpeedOrdinal), ":double_click"));
    CHECK (fired ("edit/TopBar#0/Label#" + std::to_string (practiceSpeedOrdinal), ":click"));
    CHECK (fired ("record/menu:Export...", ""));
    // Transcribe mode (issue #225): its mode button, 3 and I, and its page's overview, waveform and
    // column.
    int transcribeOrdinal = 0;
    for (int i = 0; bar.getChildComponent (i) != &bar.transcribeModeButton; ++i)
        if (dynamic_cast<const juce::TextButton*> (bar.getChildComponent (i)) != nullptr)
            ++transcribeOrdinal;
    CHECK (fired ("record/TopBar#0/TextButton#" + std::to_string (transcribeOrdinal), ":click"));
    CHECK (fired ("record/key:3", ""));
    CHECK (fired ("transcribe/key:i", ""));
    CHECK (fired ("transcribe/", "/TranscribePage#0/TranscribeOverview#0:drag"));
    CHECK (fired ("transcribe/", "/TranscribePage#0/TranscribeWave#0:pinch"));
    CHECK (fired ("transcribe/", "/TranscribePage#0/TranscribeWave#0:wheel"));
    const ui::TranscribeColumn column;
    int columnFieldOrdinal = 0;
    for (int i = 0; column.getChildComponent (i) != &column.practiceSpeedField; ++i)
        if (dynamic_cast<const juce::Label*> (column.getChildComponent (i)) != nullptr)
            ++columnFieldOrdinal;
    CHECK (fired ("transcribe/", "/TranscribeColumn#0/Label#" + std::to_string (columnFieldOrdinal) + ":double_click"));
    // A Track row's ◎, M and S in the Transcribe column (issue #226).
    for (const auto* button : { "TextButton#0", "TextButton#1", "TextButton#2" })
        CHECK (fired ("transcribe/", std::string ("/TranscribeColumn#0/TranscribeTrackRow#0/") + button + ":click"));
    // Markers (issue #227): K, each strip's flags, the empty strip, and the Markers list.
    for (const auto* scene : { "record/", "edit/", "transcribe/" })
        CHECK (fired (scene, "key:k"));
    for (const auto* gesture : { ":click", ":drag", ":double_click" })
    {
        CHECK (fired ("edit/", std::string ("/Timeline#0/MarkerStrip#0/MarkerFlag#1") + gesture));
        CHECK (fired ("transcribe/", std::string ("/TranscribePage#0/MarkerStrip#0/MarkerFlag#0") + gesture));
        CHECK (fired ("record/", std::string ("/RecordPage#0/MarkerStrip#0/MarkerFlag#0") + gesture));
    }
    CHECK (fired ("edit/", "/Timeline#0/MarkerStrip#0:click"));
    // Loops (issue #228): the strip's empty-space drag, each strip's bars, the Loop cards and the keys.
    CHECK (fired ("edit/", "/Timeline#0/MarkerStrip#0:drag"));
    CHECK (fired ("transcribe/", "/TranscribePage#0/MarkerStrip#0:drag"));
    for (const auto* gesture : { ":click", ":drag", ":double_click" })
    {
        CHECK (fired ("edit/", std::string ("/Timeline#0/MarkerStrip#0/LoopBar#1") + gesture));
        CHECK (fired ("transcribe/", std::string ("/TranscribePage#0/MarkerStrip#0/LoopBar#0") + gesture));
        CHECK (fired ("record/", std::string ("/RecordPage#0/MarkerStrip#0/LoopBar#0") + gesture));
    }
    CHECK (fired ("transcribe/", "/LoopList#0/LoopCard#0:click"));
    CHECK (fired ("transcribe/", "/LoopList#0/LoopCard#1:click"));
    for (const auto* key : { "key:[", "key:]", "key:l", "key:escape", "key:alt+left", "key:alt+right", "key:alt+1", "key:alt+2", "key:backspace" })
        for (const auto* scene : { "record/", "edit/", "transcribe/" })
            CHECK (fired (scene, key));
    CHECK (fired ("transcribe/", "/MarkerList#0/MarkerRow#1:click"));
    // Practice Speed keys (issue #229): H, - and = in every mode.
    for (const auto* key : { "key:h", "key:-", "key:=" })
        for (const auto* scene : { "record/", "edit/", "transcribe/" })
            CHECK (fired (scene, key));
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
    fireProbe (census, *plantedProbe, Scene { "record", ui::Mode::record, false }, result);

    CHECK (result.bypasses == std::set<std::string> { "record/TextButton#0:click" });
    CHECK (result.changedBy["record/TextButton#0:click"].find ("/song/song/master/volumeDb") != std::string::npos);
}
