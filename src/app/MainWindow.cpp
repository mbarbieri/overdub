#include "app/MainWindow.h"

namespace overdub::app
{
MainWindow::MainWindow (const juce::String& title)
    : DocumentWindow (title,
                      juce::Desktop::getInstance().getDefaultLookAndFeel().findColour (juce::ResizableWindow::backgroundColourId),
                      juce::DocumentWindow::allButtons)
{
    setUsingNativeTitleBar (true);
    setResizable (true, true);
    centreWithSize (1200, 800);
    setVisible (true);
}

void MainWindow::closeButtonPressed()
{
    juce::JUCEApplication::getInstance()->systemRequestedQuit();
}
} // namespace overdub::app
