cmake_minimum_required(VERSION 3.25)
project(HydraMixer VERSION 0.6.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(external/JUCE)

# RNNoise 0.2 (BSD, xiph.org) - neural denoiser built into every input strip
set(RNNOISE_DIR "${CMAKE_SOURCE_DIR}/external/rnnoise")
add_library(rnnoise STATIC
    ${RNNOISE_DIR}/src/denoise.c
    ${RNNOISE_DIR}/src/rnn.c
    ${RNNOISE_DIR}/src/pitch.c
    ${RNNOISE_DIR}/src/kiss_fft.c
    ${RNNOISE_DIR}/src/celt_lpc.c
    ${RNNOISE_DIR}/src/nnet.c
    ${RNNOISE_DIR}/src/nnet_default.c
    ${RNNOISE_DIR}/src/parse_lpcnet_weights.c
    ${RNNOISE_DIR}/src/rnnoise_data.c
    ${RNNOISE_DIR}/src/rnnoise_tables.c)
target_include_directories(rnnoise PUBLIC "${RNNOISE_DIR}/include" PRIVATE "${RNNOISE_DIR}/src")
target_compile_definitions(rnnoise PRIVATE _USE_MATH_DEFINES _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(rnnoise PRIVATE /W0 $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Oi /arch:AVX2 /fp:fast>)
endif()

juce_add_gui_app(HydraMixer
    PRODUCT_NAME "Hydra Mixer"
    COMPANY_NAME "Hydra"
    VERSION ${PROJECT_VERSION}
    ICON_BIG "${CMAKE_SOURCE_DIR}/resources/icon.png")

# The .ico is generated at configure time and the .rc is compiled without dependency scanning:
# reconfigure when the PNG changes and recompile the resources when the .ico changes,
# otherwise the exe keeps the old icon.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/resources/icon.png")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/HydraMixer_artefacts/JuceLibraryCode/HydraMixer_resources.rc"
    TARGET_DIRECTORY HydraMixer_rc_lib
    PROPERTIES OBJECT_DEPENDS "${CMAKE_CURRENT_BINARY_DIR}/HydraMixer_artefacts/JuceLibraryCode/icon.ico")

file(GLOB_RECURSE HYDRA_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/*.cpp"
    "${CMAKE_SOURCE_DIR}/src/*.h")
target_sources(HydraMixer PRIVATE ${HYDRA_SOURCES})
target_include_directories(HydraMixer PRIVATE "${CMAKE_SOURCE_DIR}/src")

target_compile_definitions(HydraMixer PRIVATE
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:HydraMixer,JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:HydraMixer,JUCE_VERSION>"
    JUCE_ASIO=1
    JUCE_WASAPI=1
    JUCE_DIRECTSOUND=0
    JUCE_PLUGINHOST_VST3=1
    JUCE_PLUGINHOST_VST=0
    JUCE_PLUGINHOST_LADSPA=0
    JUCE_PLUGINHOST_LV2=0
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_USE_CDREADER=0
    JUCE_USE_CDBURNER=0
    JUCE_USE_WINDOWS_MEDIA_FORMAT=1
    JUCE_USE_CAMERA=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN)

target_link_libraries(HydraMixer
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
        juce::juce_dsp
        rnnoise avrt ole32 uuid mmdevapi
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)

if(MSVC)
    # Realtime audio: aggressive optimisation, AVX2 vectorisation, fast float maths.
    target_compile_options(HydraMixer PRIVATE
        /W3 /permissive- /Zc:__cplusplus /MP
        $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Oi /Ot /GL /arch:AVX2 /fp:fast>)
    target_link_options(HydraMixer PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/LTCG /OPT:REF /OPT:ICF>)
endif()

# Offline DSP tests (run: build\<cfg>\HydraTests_artefacts\<cfg>\HydraTests.exe)
juce_add_console_app(HydraTests PRODUCT_NAME "HydraTests")
target_sources(HydraTests PRIVATE tests/DenoiserTest.cpp tests/SessionTest.cpp tests/MidiRangeTest.cpp tests/AppCaptureTest.cpp tests/LedTest.cpp tests/RecorderTest.cpp tests/AsioSecondaryTest.cpp tests/UndoTest.cpp tests/ReorderTest.cpp tests/SceneTest.cpp tests/SyncDelayTest.cpp tests/RoomAnalysisTest.cpp
    src/core/Denoiser.cpp src/core/RoomAnalysis.cpp src/engine/Engine.cpp src/engine/IoStreams.cpp src/engine/AsioSecondary.cpp src/engine/Recorder.cpp src/engine/PluginChain.cpp
    src/midi/MidiMapper.cpp src/midi/Hotkeys.cpp src/state/Session.cpp src/state/VoicemeeterImport.cpp src/state/UndoHistory.cpp src/state/Scenes.cpp)
target_include_directories(HydraTests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(HydraTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(HydraTests PRIVATE JUCE_ASIO=1 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0)
target_link_libraries(HydraTests PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra rnnoise avrt ole32 uuid mmdevapi
                      PUBLIC juce::juce_recommended_config_flags)
