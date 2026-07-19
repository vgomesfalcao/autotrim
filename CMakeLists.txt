cmake_minimum_required(VERSION 3.22)
project(AutoTrim VERSION 0.1.1)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

juce_add_plugin(AutoTrim
    COMPANY_NAME "vgomesfalcao"
    BUNDLE_ID com.vgomesfalcao.autotrim
    PLUGIN_MANUFACTURER_CODE Vgfa
    PLUGIN_CODE Atrm
    FORMATS AU VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    PRODUCT_NAME "AutoTrim")

target_sources(AutoTrim PRIVATE
    src/PluginProcessor.cpp
    src/PluginEditor.cpp
    src/Registry.cpp
    src/Measurement.cpp)

target_include_directories(AutoTrim PRIVATE src)

target_compile_definitions(AutoTrim PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0)

target_link_libraries(AutoTrim
    PRIVATE
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Unit tests for the pure DSP math (no JUCE dependency)
enable_testing()
add_executable(dsp_tests tests/DspTests.cpp)
target_include_directories(dsp_tests PRIVATE src)
add_test(NAME dsp_tests COMMAND dsp_tests)
