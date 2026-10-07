#include "ui/SongSession.h"

#include "ui/TrackRowEdits.h"

namespace overdub::ui
{
SongSession::SongSession (std::unique_ptr<song::SongPackage> package, AudioBinding audio)
    : package_ (std::move (package))
{
    attachAudio (std::move (audio));
}

SongSession::~SongSession()
{
    for (const auto& error : detachAudio())
        core::logError (error);
}

void SongSession::attachAudio (AudioBinding audio)
{
    binding = std::move (audio);
    if (binding.engine == nullptr)
        return;
    binding.engine->stop();
    binding.engine->setMonitoring (true); // which Tracks are heard is decided by songForEngine
    reloadEngine();
    binding.engine->seek (song().playhead);
}

std::vector<core::Error> SongSession::detachAudio()
{
    std::vector<core::Error> errors;
    if (recorder != nullptr)
    {
        binding.engine->stop();
        errors = finishPass (recorder->stop());
    }
    if (binding.engine != nullptr)
    {
        binding.engine->stop();
        binding.engine->loadSong (song::makeEmptySong (song().sampleRate), package_->location());
    }
    binding = AudioBinding {};
    return errors;
}

bool SongSession::isPlaying() const
{
    return binding.engine != nullptr && binding.engine->isPlaying();
}

song::SampleCount SongSession::position() const
{
    return binding.engine != nullptr ? binding.engine->position() : song().playhead;
}

core::Expected<void> SongSession::record()
{
    if (recorder != nullptr)
        return {};
    if (binding.engine == nullptr)
        return std::unexpected (core::Error { "ui.no_audio_device", "no audio device is open to record from", {} });

    const auto passNumber = nextPassNumber (song());
    auto started = capture::PassRecorder::start (*package_, *binding.engine, binding.latency);
    if (! started.has_value())
        return std::unexpected (started.error());

    recorder = std::move (*started);
    recordingPass = passNumber;
    if (! binding.engine->isPlaying())
        binding.engine->play();
    return {};
}

void SongSession::stop()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr)
        recorder->requestStop();
    binding.engine->stop();
}

void SongSession::playOrStop()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr || binding.engine->isPlaying())
        stop();
    else
        binding.engine->play();
}

void SongSession::rewind()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr)
        recorder->requestStop();
    binding.engine->seek (0);
}

core::Expected<void> SongSession::undo()
{
    if (recorder != nullptr)
        return {};
    auto result = package_->undo();
    reloadEngine();
    return result;
}

core::Expected<void> SongSession::redo()
{
    if (recorder != nullptr)
        return {};
    auto result = package_->redo();
    reloadEngine();
    return result;
}

void SongSession::setArmed (const song::Id& trackId, bool armed)
{
    if (recorder != nullptr)
        return;
    package_->changeWithoutHistory (withArmed (song(), trackId, armed));
    reloadEngine();
}

void SongSession::setInputChannels (const song::Id& trackId, std::vector<int> inputChannels)
{
    if (recorder != nullptr)
        return;
    package_->changeWithoutHistory (withInputChannels (song(), trackId, std::move (inputChannels)));
    reloadEngine();
}

void SongSession::setMonitoring (const song::Id& trackId, bool on)
{
    if (recorder != nullptr)
        return;
    if (on)
        monitored.insert (trackId);
    else
        monitored.erase (trackId);
    reloadEngine();
}

std::vector<core::Expected<song::Id>> SongSession::importFiles (const std::vector<juce::File>& files)
{
    if (recorder != nullptr)
        return {};
    auto results = package_->importBackingTracks (files);
    reloadEngine();
    return results;
}

std::vector<core::Error> SongSession::poll (song::SongPackage::Clock::time_point now)
{
    std::vector<core::Error> errors;
    if (recorder != nullptr && recorder->hasStopped())
        errors = finishPass (recorder->finish (now));

    if (recorder == nullptr && binding.engine != nullptr && ! binding.engine->isPlaying())
    {
        const auto position = binding.engine->position();
        if (position != song().playhead)
        {
            auto changed = song();
            changed.playhead = position;
            package_->changeWithoutHistory (std::move (changed), now);
        }
    }

    if (auto written = package_->writeIfDue (now); ! written.has_value())
        errors.push_back (written.error());
    return errors;
}

std::vector<core::Error> SongSession::finishPass (core::Expected<capture::RecordedPass> finished)
{
    recorder.reset();
    recordingPass = 0;
    reloadEngine(); // the Pass's Takes and Clips, when it was added
    if (! finished.has_value())
        return { finished.error() };
    return {};
}

void SongSession::reloadEngine()
{
    if (binding.engine != nullptr)
        binding.engine->loadSong (songForEngine (song(), monitored), package_->location());
}
} // namespace overdub::ui
