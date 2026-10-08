#overdub_plugin : Insert Chain plugins(issue #33) : AU discovery and asynchronous out - of - process
#instantiation through AudioPluginFormatManager(ADR 0002), the InsertHost that keeps each slot's
#plugin alive across render plans, and plugin state(fullStateForDocument) for the package's
#content - addressed blobs(ADR 0004).Events and Error codes use the `plugin.*` area(ADR 0006).

add_library (overdub_plugin STATIC)

    target_sources (overdub_plugin PRIVATE
                        AudioUnitIdentifier.cpp
                            AudioUnitLoader.cpp
                                InsertHost.cpp
                                    PluginState.cpp)

        target_link_libraries (overdub_plugin
                                   PUBLIC
                                       overdub_engine
                                           PRIVATE
                                               juce::juce_recommended_warning_flags)
