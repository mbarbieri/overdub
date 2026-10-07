# overdub_song: the headless Song model (plain value structs), its song.json form, format
# migrations and the .overdub package with persisted undo history and the plugin state blob store
# (ADR 0004). Events and Error codes use the `song.*` area (ADR 0006).

add_library(overdub_song STATIC)

target_sources(overdub_song PRIVATE
    Song.cpp
    SongJson.cpp
    SongMigration.cpp)

target_link_libraries(overdub_song
    PUBLIC
        overdub_core
    PRIVATE
        juce::juce_recommended_warning_flags)
