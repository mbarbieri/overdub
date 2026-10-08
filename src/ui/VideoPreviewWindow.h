#pragma once

#include "ui/VideoScreen.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>

// Edit mode's floating Video Preview (issue #36, prototype variant C): a small resizable window
// holding a VideoScreen. The Edit toolbar's "Video" toggle shows and hides it (MainComponent); its
// close button asks to hide it (onClose). Built once and kept, hidden, so it reopens where it was.
//
// Message thread only.
namespace overdub::ui
{
class VideoPreviewWindow final : public juce::DocumentWindow
{
public:
    static constexpr int defaultWidth = 360;
    static constexpr int defaultHeight = 240;

    // onDesktop false keeps the window off-screen (tests): it is built, laid out and its visible
    // flag follows setShown, but it never reaches the desktop.
    explicit VideoPreviewWindow (bool onDesktop);
    ~VideoPreviewWindow() override;

    void setShown (bool shown);
    bool isShown() const { return isVisible(); }

    VideoScreen& screen() { return *videoScreen; }

    void closeButtonPressed() override;

    std::function<void()> onClose;

private:
    VideoScreen* videoScreen = nullptr; // owned as the content component
    bool onDesktop = true;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VideoPreviewWindow)
};
} // namespace overdub::ui
