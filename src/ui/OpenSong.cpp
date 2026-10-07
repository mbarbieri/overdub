#include "ui/OpenSong.h"

#include "ui/TrackRowEdits.h"

#include <algorithm>

namespace overdub::ui
{
OpenSong::OpenSong (std::unique_ptr<song::SongPackage> package, AudioBinding audio)
    : package_ (std::move (package))
{
    attachAudio (std::move (audio));
}

OpenSong::~OpenSong()
{
    for (const auto& error : detachAudio())
        core::logError (error);
}

void OpenSong::attachAudio (AudioBinding audio)
{
    binding = std::move (audio);
    if (binding.engine == nullptr)
        return;
    binding.engine->stop();
    binding.engine->setMonitoring (true); // which Tracks are heard is decided by songForEngine
    reloadEngine();
    binding.engine->seek (song().playhead);
}

std::vector<core::Error> OpenSong::detachAudio()
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

bool OpenSong::isPlaying() const
{
    return binding.engine != nullptr && binding.engine->isPlaying();
}

song::SampleCount OpenSong::position() const
{
    return binding.engine != nullptr ? binding.engine->position() : song().playhead;
}

core::Expected<void> OpenSong::record()
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

void OpenSong::stop()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr)
        recorder->requestStop();
    binding.engine->stop();
}

void OpenSong::playOrStop()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr || binding.engine->isPlaying())
        stop();
    else
        binding.engine->play();
}

void OpenSong::rewind()
{
    if (binding.engine == nullptr)
        return;
    if (recorder != nullptr)
        recorder->requestStop();
    binding.engine->seek (0);
}

void OpenSong::seek (song::SampleCount sample)
{
    if (recorder != nullptr)
        return;
    const auto position = std::max<song::SampleCount> (0, sample);
    if (binding.engine != nullptr)
    {
        binding.engine->seek (position);
        return;
    }
    auto changed = song();
    changed.playhead = position;
    package_->changeWithoutHistory (std::move (changed));
}

core::Expected<void> OpenSong::edit (song::Song changed, std::string label)
{
    if (recorder != nullptr)
        return {};
    auto result = package_->edit (std::move (changed), std::move (label));
    reloadEngine();
    return result;
}

const song::Peaks& OpenSong::peaks (const song::ClipSource& source)
{
    if (const auto cached = peaksByMediaId.find (source.id); cached != peaksByMediaId.end())
        return cached->second;

    auto loaded = package_->peaks (source);
    if (! loaded.has_value())
        core::logError (loaded.error(), core::Level::warn);
    return peaksByMediaId[source.id] = loaded.value_or (song::Peaks {});
}

core::Expected<void> OpenSong::undo()
{
    if (recorder != nullptr)
        return {};
    auto result = package_->undo();
    reloadEngine();
    return result;
}

core::Expected<void> OpenSong::redo()
{
    if (recorder != nullptr)
        return {};
    auto result = package_->redo();
    reloadEngine();
    return result;
}

void OpenSong::setArmed (const song::Id& trackId, bool armed)
{
    if (recorder != nullptr)
        return;
    package_->changeWithoutHistory (withArmed (song(), trackId, armed));
    reloadEngine();
}

void OpenSong::setInputChannels (const song::Id& trackId, std::vector<int> inputChannels)
{
    if (recorder != nullptr)
        return;
    package_->changeWithoutHistory (withInputChannels (song(), trackId, std::move (inputChannels)));
    reloadEngine();
}

void OpenSong::setMonitoring (const song::Id& trackId, bool on)
{
    if (recorder != nullptr)
        return;
    if (on)
        monitored.insert (trackId);
    else
        monitored.erase (trackId);
    reloadEngine();
}

std::vector<core::Expected<song::Id>> OpenSong::importFiles (const std::vector<juce::File>& files)
{
    if (recorder != nullptr)
        return {};
    auto results = package_->importBackingTracks (files);
    reloadEngine();
    return results;
}

std::vector<core::Error> OpenSong::poll (song::SongPackage::Clock::time_point now)
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

std::vector<core::Error> OpenSong::finishPass (core::Expected<capture::RecordedPass> finished)
{
    recorder.reset();
    recordingPass = 0;
    reloadEngine(); // the Pass's Takes and Clips, when it was added
    if (! finished.has_value())
        return { finished.error() };
    return {};
}

void OpenSong::reloadEngine()
{
    if (binding.engine != nullptr)
        binding.engine->loadSong (songForEngine (song(), monitored), package_->location());
}
} // namespace overdub::ui
