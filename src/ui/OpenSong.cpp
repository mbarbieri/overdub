#include "ui/OpenSong.h"

#include "core/Log.h"
#include "song/InsertEdits.h"
#include "song/MixEdits.h"
#include "ui/TrackRowEdits.h"

#include <algorithm>

namespace overdub::ui
{
namespace
{
// Plugins are prepared for this block size when no device is open.
constexpr int blockSizeWithoutDevice = 512;
} // namespace

OpenSong::OpenSong (std::unique_ptr<song::SongPackage> package, AudioBinding audio, plugin::Instantiator instantiate)
    : package_ (std::move (package)),
      inserts (std::move (instantiate),
               [this] (const std::string& hash) { return package_->loadPluginState (hash); },
               package_->song().sampleRate,
               blockSizeOf (audio))
{
    package_->setPluginStateCapture ([this] (song::Song& song) { capturePluginState (song); });
    inserts.sync (song());
    attachAudio (std::move (audio));
}

OpenSong::~OpenSong()
{
    for (const auto& error : detachAudio())
        core::logError (error);
    if (inserts.takeStateChanged())
        package_->changeWithoutHistory (song());
    if (auto written = package_->flush(); ! written.has_value())
        core::logError (written.error());
    package_->setPluginStateCapture ({});
}

int OpenSong::blockSizeOf (const AudioBinding& audio)
{
    return audio.engine != nullptr ? audio.engine->configuration().maxBlockSize : blockSizeWithoutDevice;
}

void OpenSong::capturePluginState (song::Song& song)
{
    auto captured = inserts.captureStates (song, [this] (const juce::MemoryBlock& state) { return package_->storePluginState (state); });
    if (! captured.has_value())
        core::logError (captured.error(), core::Level::warn);
}

void OpenSong::attachAudio (AudioBinding audio)
{
    binding = std::move (audio);
    if (binding.engine == nullptr)
        return;
    // No engine renders the plugins now: the previous one was detached and is gone.
    inserts.prepare (song().sampleRate, blockSizeOf (binding));
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
    const auto preRollSamples = (song::SampleCount) song::preRollSeconds (song().preRoll) * song().sampleRate;
    const auto usePreRoll = ! binding.engine->isPlaying() && preRollSamples > 0;
    const auto recordPoint = binding.engine->position();

    capture::PassRecorderOptions options;
    if (usePreRoll)
        options.recordPoint = recordPoint;
    auto started = capture::PassRecorder::start (*package_, *binding.engine, binding.latency, options);
    if (! started.has_value())
        return std::unexpected (started.error());

    recorder = std::move (*started);
    recordingPass = passNumber;
    preRollPoint = options.recordPoint;
    // A Mixer preview still applied (a drag the Pass interrupts) gives way to the Song's mix.
    binding.engine->setMix (song());
    if (usePreRoll)
    {
        const auto playFrom = std::max<song::SampleCount> (0, recordPoint - preRollSamples);
        binding.engine->seek (playFrom);
        core::logEvent (core::Level::info, "ui.pre_roll_started", { { "record_point", recordPoint }, { "play_from", playFrom }, { "pre_roll_seconds", song::preRollSeconds (song().preRoll) } });
    }
    if (! binding.engine->isPlaying())
        binding.engine->play();
    return {};
}

std::optional<song::SampleCount> OpenSong::preRollRecordPoint() const
{
    if (recorder == nullptr || ! preRollPoint.has_value() || recorder->hasStopped() || recorder->runningPassStart().has_value())
        return std::nullopt;
    return preRollPoint;
}

std::optional<song::SampleCount> OpenSong::runningPassStart() const
{
    return recorder != nullptr ? recorder->runningPassStart() : std::nullopt;
}

void OpenSong::setPreRoll (song::PreRoll preRoll)
{
    if (recorder != nullptr || preRoll == song().preRoll)
        return;
    auto changed = song();
    changed.preRoll = preRoll;
    package_->changeWithoutHistory (std::move (changed));
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
    const auto before = song();
    auto result = package_->edit (std::move (changed), std::move (label));
    updateEngine (before);
    return result;
}

core::Expected<void> OpenSong::addInsert (const song::Id& trackId, const plugin::AvailableAudioUnit& plugin)
{
    if (recorder != nullptr)
        return {};
    const song::InsertSlot slot { .id = song::makeId(), .component = plugin.component, .name = plugin.name, .version = plugin.version };
    auto changed = song::addInsert (song(), trackId, slot);
    if (! changed.has_value())
        return std::unexpected (changed.error());
    return edit (std::move (*changed), song::addInsertLabel (song(), trackId, plugin.name));
}

void OpenSong::previewMix (const song::Song& candidate)
{
    if (recorder != nullptr || binding.engine == nullptr || ! song::differsOnlyInMix (song(), candidate))
        return;
    binding.engine->setMix (candidate);
}

engine::MeterPeaks OpenSong::meterPeaks()
{
    return binding.engine != nullptr ? binding.engine->takeMeterPeaks() : engine::MeterPeaks {};
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
    const auto before = song();
    auto result = package_->undo();
    updateEngine (before);
    return result;
}

core::Expected<void> OpenSong::redo()
{
    if (recorder != nullptr)
        return {};
    const auto before = song();
    auto result = package_->redo();
    updateEngine (before);
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

    // During a Pass the plan stays as it started; finishPass rebuilds it.
    if (recorder == nullptr && inserts.takeGraphChanged())
        reloadEngine();
    if (inserts.takeStateChanged())
        package_->changeWithoutHistory (song(), now);

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
    preRollPoint.reset();
    reloadEngine(); // the Pass's Takes and Clips, when it was added
    if (! finished.has_value())
        return { finished.error() };
    return {};
}

void OpenSong::updateEngine (const song::Song& before)
{
    if (binding.engine != nullptr && song::differsOnlyInMix (before, song()) && binding.engine->setMix (song()))
        return;
    reloadEngine();
}

void OpenSong::reloadEngine()
{
    inserts.sync (song());
    if (binding.engine != nullptr)
        binding.engine->loadSong (songForEngine (song(), monitored), package_->location(), inserts.insertFactory());
}
} // namespace overdub::ui
