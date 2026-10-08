#include "ui/VideoPreviewWindow.h"

#include "ui/Colours.h"

namespace overdub::ui
{
VideoPreviewWindow::VideoPreviewWindow (bool showOnDesktop)
    : DocumentWindow ("Video Preview", colours::background, juce::DocumentWindow::closeButton, false),
      onDesktop (showOnDesktop)
{
    setUsingNativeTitleBar (true);
    auto content = std::make_unique<VideoScreen>();
    content->setSize (defaultWidth, defaultHeight);
    videoScreen = content.get();
    setContentOwned (content.release(), true);
    setResizable (true, false);
    setResizeLimits (160, 90, 1920, 1080);
    if (onDesktop)
        centreWithSize (getWidth(), getHeight());
}

VideoPreviewWindow::~VideoPreviewWindow()
{
    videoScreen = nullptr;
    clearContentComponent();
}

void VideoPreviewWindow::setShown (bool shown)
{
    if (shown == isVisible())
        return;
    if (shown && onDesktop && ! isOnDesktop())
        addToDesktop (getDesktopWindowStyleFlags());
    setVisible (shown);
    if (shown && onDesktop)
        toFront (false);
}

void VideoPreviewWindow::closeButtonPressed()
{
    if (onClose)
        onClose();
}
} // namespace overdub::ui
