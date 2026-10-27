# overdub_song: the headless Song model (plain value structs), its song.json form, format
# migrations and the .overdub package with persisted undo history and the plugin state blob store
# (ADR 0004), Backing Track import (Core Audio ExtAudioFile), the peaks cache and the Transpose
# and Practice Speed renders (Signalsmith Stretch, ADR 0009). Events and Error
# codes use the `song.*` area (ADR 0006).

add_library(overdub_song STATIC)

target_sources(overdub_song PRIVATE
    AudioFileDecode.cpp
    BackingTrackImport.cpp
    ClipEdits.cpp
    InsertEdits.cpp
    MixEdits.cpp
    TakeEdits.cpp
    TrackColours.cpp
    TrackEdits.cpp
    VideoTakeEdits.cpp
    Peaks.cpp
    PracticeSpeed.cpp
    RenderWorker.cpp
    SampleRateConversion.cpp
    Separation.cpp
    SeparationHelper.cpp
    SeparationModel.cpp
    SeparationRun.cpp
    Song.cpp
    SongJson.cpp
    SongMigration.cpp
    SongHistory.cpp
    SongPackage.cpp
    Transpose.cpp
    TransposeRender.cpp
    AtomicWrite.cpp
    PluginBlobStore.cpp)

target_link_libraries(overdub_song
    PUBLIC
        overdub_core
        "-framework AudioToolbox"
        "-framework CoreFoundation"
    PRIVATE
        overdub_signalsmith_stretch
        juce::juce_recommended_warning_flags)
