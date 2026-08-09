juce_add_plugin(SP404Companion
    COMPANY_NAME "TODO"
    PLUGIN_MANUFACTURER_CODE Sp4C
    PLUGIN_CODE Cmp1
    FORMATS VST3 AU
    PRODUCT_NAME "SP404 Companion"
    NEEDS_WEB_BROWSER TRUE
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
)

juce_add_binary_data(SP404CompanionWebResources SOURCES
    web/index.html
    web/styles.css
    web/themes.css
    web/app.js
    web/header.png
    web/screen.png
    web/knob0.png
    web/knob1.png
    web/knob2.png
    web/knob3.png
    web/knob4.png
    web/knob5.png
    web/knob6.png
    # Decorative stickers for the "Kawaii"/"Japandi" themes (see themes.css, app.js's
    # kStickerLayouts) -- cut from user-provided generic sticker sheets (no third-party
    # characters/logos, see README) via a flood-fill + connected-component script.
    web/stickers/jp1.png
    web/stickers/jp2.png
    web/stickers/jp3.png
    web/stickers/jp4.png
    web/stickers/jp5.png
    web/stickers/jp6.png
    web/stickers/jp7.png
    web/stickers/jp8.png
    web/stickers/jp9.png
    web/stickers/jp10.png
    web/stickers/jp11.png
    web/stickers/jp12.png
    web/stickers/jp13.png
    web/stickers/kawaii1.png
    web/stickers/kawaii2.png
    web/stickers/kawaii3.png
    web/stickers/kawaii4.png
    web/stickers/kawaii5.png
    web/stickers/kawaii6.png
    web/stickers/kawaii7.png
    web/stickers/kawaii8.png
    web/stickers/kawaii9.png
    web/stickers/kawaii10.png
    web/stickers/kawaii11.png
    web/stickers/kawaii12.png
)

target_compile_definitions(SP404Companion PUBLIC
    # No VST2 build/release exists or is planned, so there's no automation-compatibility
    # concern between VST2 and VST3 parameter IDs.
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_sources(SP404Companion PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
    WebUIBridge.cpp
    BankLoader.cpp
    SampleImport.cpp
    BankArchive.cpp
    SampleDsp.cpp
    "${rubberband_SOURCE_DIR}/single/RubberBandSingle.cpp"
)

target_include_directories(SP404Companion PRIVATE "${rubberband_SOURCE_DIR}")

target_link_libraries(SP404Companion
    PRIVATE
        sp404_core
        SP404CompanionWebResources
        juce::juce_audio_utils
        juce::juce_audio_formats
        juce::juce_gui_extra
        juce::juce_dsp
        "-framework Accelerate" # Rubber Band's single-file build uses the vDSP FFT on Apple platforms
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
