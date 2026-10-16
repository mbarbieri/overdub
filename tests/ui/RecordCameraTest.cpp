#include "ui/RecordPage.h"

#include "ui/MainWindowFixture.h"

#include <catch2/catch_test_macros.hpp>

// The camera arm, the camera picker and the stage in Record mode (issue #35), through the real
// window over a fake camera: asserts on the Song, the camera, what the controls and the stage show,
// and the Notices.
using overdub::core::ScopedLogCapture;
using namespace overdub::ui;
using namespace overdub::ui::testing;

namespace
{
std::vector<nlohmann::json> eventsNamed (const ScopedLogCapture& capture, const std::string& name)
{
    std::vector<nlohmann::json> found;
    for (const auto& event : capture.events())
        if (event.value ("event", "") == name)
            found.push_back (event);
    return found;
}

struct CameraWindowFixture : MainWindowFixture
{
    CameraWindowFixture()
    {
        window->tick();
        REQUIRE (isShown (page()));
    }

    RecordPage& page() { return window->recordPage; }
};
} // namespace

TEST_CASE ("the camera arm and picker sit above the stage, which shows Video Take playback while disarmed", "[ui][record_page][camera]")
{
    CameraWindowFixture f;
    CHECK (isShown (f.page().cameraArmButton));
    CHECK (isShown (f.page().cameraPicker));
    // Disarmed, the stage shows playback (issue #36): no Video Take here, so its message.
    CHECK_FALSE (isShown (f.page().cameraStage));
    CHECK (isShown (f.page().stageVideo));
    CHECK (f.page().stageVideo.message() == juce::String (noVideoTakeMessage));
    CHECK (f.page().cameraArmButton.getBottom() < f.page().cameraStage.getY());
    CHECK_FALSE (f.page().cameraArmButton.getToggleState());
    CHECK (f.page().cameraPicker.getNumItems() == 2);
    CHECK (f.page().cameraPicker.getItemText (1) == "USB Camera");
    CHECK (f.page().cameraStage.message() == juce::String (cameraStageMessage (false)));
    CHECK_FALSE (f.page().cameraStage.showsFeed());
}

TEST_CASE ("clicking the camera arm arms the Song's camera without an undo step, and again disarms it", "[ui][record_page][camera]")
{
    CameraWindowFixture f;
    const auto undoBefore = f.openSong->package().undoLabel();
    click (f.page().cameraArmButton);

    CHECK (f.song().settings.cameraArmed);
    CHECK (f.song().settings.cameraDeviceId == "camera-facetime"); // the default camera became the choice
    REQUIRE (f.camera.opened.has_value());
    CHECK (f.openSong->package().undoLabel() == undoBefore);
    CHECK (f.page().cameraArmButton.getToggleState());
    CHECK (f.page().cameraPicker.getSelectedItemIndex() == 0);
    CHECK (f.page().cameraStage.message() == juce::String (cameraStageMessage (true)));

    click (f.page().cameraArmButton);
    CHECK_FALSE (f.song().settings.cameraArmed);
    CHECK_FALSE (f.camera.opened.has_value());
}

TEST_CASE ("picking a camera makes it the Song's camera", "[ui][record_page][camera]")
{
    CameraWindowFixture f;
    click (f.page().cameraArmButton);
    choose (f.page().cameraPicker, 1);

    CHECK (f.song().settings.cameraDeviceId == "camera-usb");
    REQUIRE (f.camera.opened.has_value());
    CHECK (f.camera.opened->id == "camera-usb");
    CHECK (f.page().cameraPicker.getSelectedItemIndex() == 1);
}

TEST_CASE ("refused camera access leaves the camera disarmed and shows a Notice", "[ui][record_page][camera]")
{
    ScopedLogCapture capture;
    CameraWindowFixture f;
    f.cameraAccessGranted = false;
    click (f.page().cameraArmButton);

    CHECK_FALSE (f.song().settings.cameraArmed);
    CHECK_FALSE (f.camera.opened.has_value());
    const auto notices = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (notices.size() == 1);
    CHECK (notices[0]["code"] == "capture.camera_access_denied");
    CHECK (isShown (f.window->noticeLine));
}

TEST_CASE ("a camera that cannot open shows a Notice and stays disarmed", "[ui][record_page][camera]")
{
    ScopedLogCapture capture;
    CameraWindowFixture f;
    f.camera.openError = overdub::core::Error { "capture.camera_unavailable", "the camera could not be opened", { { "camera_id", "" }, { "reason", "no camera found" } } };
    click (f.page().cameraArmButton);

    CHECK_FALSE (f.song().settings.cameraArmed);
    CHECK_FALSE (f.page().cameraArmButton.getToggleState());
    const auto notices = eventsNamed (capture, "ui.notice_shown");
    REQUIRE (notices.size() == 1);
    CHECK (notices[0]["code"] == "capture.camera_unavailable");
}

TEST_CASE ("with the camera Armed a Pass from the window adds a Video Take, and the controls lock during it", "[ui][record_page][camera]")
{
    CameraWindowFixture f;
    click (f.page().cameraArmButton);
    f.camera.movie.firstFrameHostNs = f.device.hostTimeNs + 100'000'000ULL;
    f.camera.movie.frames = 20;

    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (200);
    REQUIRE (f.openSong->isRecording());
    CHECK_FALSE (f.page().cameraArmButton.isEnabled());
    CHECK_FALSE (f.page().cameraPicker.isEnabled());
    REQUIRE (f.camera.writing.has_value());

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);
    REQUIRE_FALSE (f.openSong->isRecording());
    REQUIRE (f.song().videoTakes.size() == 1);
    CHECK (f.song().videoTakes[0].passId == f.song().passes.back().id);
    CHECK (f.page().cameraArmButton.isEnabled());
}

TEST_CASE ("the top bar stops reading REC once the Pass is stopped, while its movie completes, and gestures stay refused", "[ui][record_page][camera]")
{
    CameraWindowFixture f;
    click (f.page().cameraArmButton);
    f.camera.movie.firstFrameHostNs = f.device.hostTimeNs + 100'000'000ULL;
    f.camera.movie.frames = 20;
    f.camera.finishGate = std::make_shared<overdub::capture::testing::MovieGate>();

    REQUIRE (pressKey (*f.window, 'r'));
    f.runBlocks (200);
    CHECK (f.window->topBar.clock.statusText() == juce::String (juce::CharPointer_UTF8 ("REC \xc2\xb7 PASS 1")));
    CHECK (f.window->topBar.recordButton.getToggleState());

    REQUIRE (pressKey (*f.window, juce::KeyPress::spaceKey));
    f.runBlocks (1);

    // Stopped, the movie not complete: the Pass is not added yet, and the bar says it is saving.
    REQUIRE (f.openSong->isRecording());
    CHECK (f.song().passes.empty());
    CHECK (f.window->topBar.clock.statusText() == juce::String (juce::CharPointer_UTF8 ("SAVING \xc2\xb7 PASS 1")));
    CHECK_FALSE (f.window->topBar.recordButton.getToggleState());
    // Gestures that must wait for the Pass to be added stay refused.
    CHECK_FALSE (f.window->topBar.undoButton.isEnabled());
    CHECK_FALSE (f.page().cameraArmButton.isEnabled());

    f.camera.finishGate->release();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (5);
    while (f.openSong->isRecording() && std::chrono::steady_clock::now() < deadline)
    {
        f.runBlocks (1);
        std::this_thread::sleep_for (std::chrono::milliseconds (1));
    }
    REQUIRE_FALSE (f.openSong->isRecording());
    CHECK (f.window->topBar.clock.statusText() == "STOP");
    CHECK (f.song().passes.size() == 1);
}

TEST_CASE ("the camera controls follow the Song and the camera list", "[ui][record_page][camera]")
{
    overdub::song::Song song;
    song.settings.cameraArmed = true;
    song.settings.cameraDeviceId = "camera-usb";
    const std::vector<overdub::capture::CameraDevice> cameras { { "camera-facetime", "FaceTime HD Camera" }, { "camera-usb", "USB Camera" } };

    const auto armed = cameraControlsStateFor (song, cameras, false);
    CHECK (armed.armed);
    CHECK (armed.selected == std::optional<std::size_t> { 1 });
    CHECK (armed.canArm);
    CHECK (armed.canChoose);

    const auto recording = cameraControlsStateFor (song, cameras, true);
    CHECK_FALSE (recording.canArm);
    CHECK_FALSE (recording.canChoose);

    song.settings.cameraArmed = false;
    song.settings.cameraDeviceId = "camera-gone";
    const auto noCameras = cameraControlsStateFor (song, {}, false);
    CHECK_FALSE (noCameras.selected.has_value());
    CHECK_FALSE (noCameras.canArm); // nothing to arm
}
