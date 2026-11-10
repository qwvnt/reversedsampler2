cmake_minimum_required(VERSION 3.22)

# Minimum macOS version for the built plug-in (ignored on other platforms).
set(CMAKE_OSX_DEPLOYMENT_TARGET "10.13" CACHE STRING "Minimum macOS version")

project(ReverseSampler VERSION 0.20.0)

# -----------------------------------------------------------------------------
# JUCE
# By default JUCE is downloaded from GitHub during the first configure.
# To use a local copy instead:  cmake -B build -DLOCAL_JUCE_PATH=/path/to/JUCE
# -----------------------------------------------------------------------------
set(RS_JUCE_VERSION "9.0.5")
set(LOCAL_JUCE_PATH "" CACHE PATH "Optional path to a local JUCE ${RS_JUCE_VERSION} folder")
option(RS_COPY_PLUGIN_AFTER_BUILD "Copy the plug-ins to the system plug-in folders after each build" OFF)
option(RS_BUILD_TESTS "Also build the ReverseSamplerTests console app" OFF)

if(LOCAL_JUCE_PATH)
    add_subdirectory("${LOCAL_JUCE_PATH}" "${CMAKE_CURRENT_BINARY_DIR}/JUCE")
else()
    include(FetchContent)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    message(STATUS "Getting JUCE ${RS_JUCE_VERSION} (downloaded on the first configure only)...")
    FetchContent_Declare(juce
        URL "https://github.com/juce-framework/JUCE/archive/refs/tags/${RS_JUCE_VERSION}.tar.gz")
    FetchContent_MakeAvailable(juce)
endif()

# -----------------------------------------------------------------------------
# Plug-in
# Keep PLUGIN_MANUFACTURER_CODE and PLUGIN_CODE unchanged once the plug-in is
# used in DAW projects: hosts identify the plug-in by these codes.
# -----------------------------------------------------------------------------
juce_add_plugin(ReverseSampler
    PRODUCT_NAME "Reverse Sampler"
    COMPANY_NAME "Shinjitsu"
    BUNDLE_ID "com.shinjitsu.reversesampler"
    PLUGIN_MANUFACTURER_CODE Shnj
    PLUGIN_CODE Rvsm
    FORMATS VST3 AU Standalone              # AU is only built on macOS
    IS_SYNTH TRUE
    NEEDS_MIDI_INPUT TRUE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES Instrument Sampler
    AU_MAIN_TYPE kAudioUnitType_MusicDevice
    NEEDS_WEB_BROWSER FALSE
    NEEDS_CURL FALSE
    COPY_PLUGIN_AFTER_BUILD ${RS_COPY_PLUGIN_AFTER_BUILD})

set(RS_SOURCES
    Source/PluginProcessor.h
    Source/PluginProcessor.cpp
    Source/SamplerEngine.h
    Source/SamplerEngine.cpp
    Source/LayerPlayer.cpp
    Source/EqDsp.h
    Source/DriveDsp.h
    Source/ReverbDsp.h
    Source/ChorusDsp.h
    Source/ModSources.h
    Source/ModMatrix.h
    Source/ModMatrix.cpp
    Source/ModMatrixEditor.h
    Source/ModMatrixEditor.cpp
    Source/CompressorDsp.h
    Source/FxRack.h
    Source/FxRack.cpp
    Source/FxRackEditor.h
    Source/FxRackEditor.cpp
    Source/LayerBus.h
    Source/LayerBus.cpp
    Source/PluginEditor.h
    Source/PluginEditor.cpp
    Source/LayerPanel.h
    Source/LayerPanel.cpp
    Source/SampleView.h
    Source/SampleView.cpp
    Source/LayerEditor.h
    Source/LayerEditor.cpp
    Source/LayerText.h
    Source/WaveformView.h
    Source/WaveformView.cpp
    Source/EnvelopeEditor.h
    Source/EnvelopeEditor.cpp
    Source/TimelineView.h
    Source/TimelineView.cpp
    Source/SampleEditor.h
    Source/SampleEditor.cpp
    Source/Controls.h
    Source/Theme.h)
list(TRANSFORM RS_SOURCES PREPEND "${CMAKE_CURRENT_SOURCE_DIR}/")

target_sources(ReverseSampler PRIVATE ${RS_SOURCES})
source_group(TREE "${CMAKE_CURRENT_SOURCE_DIR}" FILES ${RS_SOURCES})

# Signalsmith Stretch (MIT): time-stretching and pitch-shifting, header-only.
target_include_directories(ReverseSampler SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/signalsmith-stretch/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/signalsmith-linear/include")
target_compile_options(ReverseSampler PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)

target_compile_definitions(ReverseSampler
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_USE_MP3AUDIOFORMAT=1)

target_link_libraries(ReverseSampler
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)

# Visual Studio: F5 starts the standalone app.
set_property(DIRECTORY PROPERTY VS_STARTUP_PROJECT ReverseSampler_Standalone)

# -----------------------------------------------------------------------------
# Tests (optional): cmake -B build -DRS_BUILD_TESTS=ON, then run
# build/ReverseSamplerTests_artefacts/<Config>/ReverseSamplerTests
# The test app compiles the plug-in sources itself, so it needs no plug-in host.
# -----------------------------------------------------------------------------
if(RS_BUILD_TESTS)
    enable_testing()
    add_test(NAME ReverseSamplerAudioRegression COMMAND ReverseSamplerTests)
    # The EQ core has no JUCE dependency: a fast, standalone accuracy/safety test.
    add_executable(EqDspTests "${CMAKE_CURRENT_SOURCE_DIR}/Tests/EqDspTests.cpp")
    target_compile_features(EqDspTests PRIVATE cxx_std_17)
    target_include_directories(EqDspTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    add_test(NAME EqDsp COMMAND EqDspTests)
    add_executable(DriveDspTests "${CMAKE_CURRENT_SOURCE_DIR}/Tests/DriveDspTests.cpp")
    target_compile_features(DriveDspTests PRIVATE cxx_std_17)
    target_include_directories(DriveDspTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    add_test(NAME DriveDsp COMMAND DriveDspTests)
    add_executable(ChorusDspTests "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ChorusDspTests.cpp")
    target_compile_features(ChorusDspTests PRIVATE cxx_std_17)
    target_include_directories(ChorusDspTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    add_test(NAME ChorusDsp COMMAND ChorusDspTests)
    add_executable(CompressorDspTests "${CMAKE_CURRENT_SOURCE_DIR}/Tests/CompressorDspTests.cpp")
    target_compile_features(CompressorDspTests PRIVATE cxx_std_17)
    target_include_directories(CompressorDspTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    add_test(NAME CompressorDsp COMMAND CompressorDspTests)
    add_executable(ReverbDspTests "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReverbDspTests.cpp")
    target_compile_features(ReverbDspTests PRIVATE cxx_std_17)
    target_include_directories(ReverbDspTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    add_test(NAME ReverbDsp COMMAND ReverbDspTests)
    add_executable(IdleGateTests "${CMAKE_CURRENT_SOURCE_DIR}/Tests/IdleGateTests.cpp")
    target_compile_features(IdleGateTests PRIVATE cxx_std_17)
    target_include_directories(IdleGateTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    add_test(NAME IdleGate COMMAND IdleGateTests)
    # The FX rack against a minimal JUCE stand-in (Tests/MockJuce). Not in the default build:
    # cmake --build build --target FxRackTests
    add_executable(FxRackTests EXCLUDE_FROM_ALL "${CMAKE_CURRENT_SOURCE_DIR}/Tests/FxRackTests.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/Source/FxRack.cpp")
    target_compile_features(FxRackTests PRIVATE cxx_std_17)
    target_include_directories(FxRackTests BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Tests/MockJuce" "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    set_tests_properties(ReverbDsp PROPERTIES TIMEOUT 600)
    set_tests_properties(ReverseSamplerAudioRegression PROPERTIES TIMEOUT 600)
    juce_add_console_app(ReverseSamplerTests PRODUCT_NAME "ReverseSamplerTests")
    target_sources(ReverseSamplerTests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/Tests/ReverseSamplerTests.cpp"
        ${RS_SOURCES})
    target_include_directories(ReverseSamplerTests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/Source")
    target_include_directories(ReverseSamplerTests SYSTEM PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/signalsmith-stretch/include"
        "${CMAKE_CURRENT_SOURCE_DIR}/ThirdParty/signalsmith-linear/include")
    target_compile_options(ReverseSamplerTests PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/bigobj>)
    target_compile_definitions(ReverseSamplerTests PRIVATE
        JucePlugin_Name="Reverse Sampler"
        JucePlugin_VersionString="${PROJECT_VERSION}"
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_USE_MP3AUDIOFORMAT=1
        JUCE_MODAL_LOOPS_PERMITTED=1)
    target_link_libraries(ReverseSamplerTests PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
endif()
