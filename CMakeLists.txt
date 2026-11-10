cmake_minimum_required(VERSION 3.25)
project(HydraMixer VERSION 0.8.1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_subdirectory(external/JUCE)

# HYDRA_FAST: quick rebuilds while iterating (build.ps1 -Fast). Same optimisation per file, but no link-time
# optimisation (it redoes the whole program, JUCE included, on every link) and incremental linking.
option(HYDRA_FAST "Quick rebuilds: no link-time optimisation, incremental linking" OFF)

# Precompiled JUCE and standard headers, so that each of our files does not parse them again. JUCE's own
# module sources must stay out: they set up their own defines before including anything.
file(GLOB HYDRA_JUCE_SOURCES "${CMAKE_SOURCE_DIR}/external/JUCE/modules/*/*.cpp" "${CMAKE_SOURCE_DIR}/external/JUCE/modules/*/*.c")
set_source_files_properties(${HYDRA_JUCE_SOURCES} PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
function(hydra_precompile_headers target)
    target_precompile_headers(${target} PRIVATE
        <juce_audio_utils/juce_audio_utils.h>
        <juce_audio_processors/juce_audio_processors.h>
        <juce_audio_devices/juce_audio_devices.h>
        <juce_gui_extra/juce_gui_extra.h>
        <array> <atomic> <complex> <memory> <vector>)
endfunction()

# RNNoise 0.2 (BSD, xiph.org) - neural denoiser built into every input strip
set(RNNOISE_DIR "${CMAKE_SOURCE_DIR}/external/rnnoise")
add_library(rnnoise STATIC EXCLUDE_FROM_ALL
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

# HYDRA_PLUGIN_ONLY: just the plugin (build.ps1 builds it in a folder without "&" in its path, see below).
option(HYDRA_PLUGIN_ONLY "Build only the Room Correction plugin" OFF)
if(NOT HYDRA_PLUGIN_ONLY)
# libopus 1.5.2 (BSD, xiph.org, release archive unchanged) - compression for Hydra Link (audio over the network).
# Float build with runtime CPU detection; no programs, tests, DNN extras (DRED / OSCE / deep PLC) or install rules.
set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(OPUS_BUILD_SHARED_LIBRARY OFF CACHE BOOL "" FORCE)
set(OPUS_DRED OFF CACHE BOOL "" FORCE)
set(OPUS_OSCE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
add_subdirectory(external/opus EXCLUDE_FROM_ALL)
if(MSVC)
    target_compile_options(opus PRIVATE /W0)
endif()

# ONNX Runtime with DirectML (MIT) + DirectML (Microsoft, redistributable) for the Stems effect (src/engine/Stems.*).
# Fetched by tools/fetch-onnxruntime.ps1 into external/onnxruntime (not in git). The DLLs are loaded at run time from
# next to the exe (no import library), so the app also starts without them; without the folder Stems is compiled out.
set(HYDRA_ORT_DIR "${CMAKE_SOURCE_DIR}/external/onnxruntime")
if(EXISTS "${HYDRA_ORT_DIR}/include/onnxruntime_c_api.h" AND EXISTS "${HYDRA_ORT_DIR}/bin/onnxruntime.dll")
    set(HYDRA_STEMS 1)
    set(HYDRA_ORT_RC "${CMAKE_BINARY_DIR}/hydra_ort.rc")
    file(WRITE "${HYDRA_ORT_RC}.in"
        "201 RCDATA \"${HYDRA_ORT_DIR}/bin/onnxruntime.dll\"\n202 RCDATA \"${HYDRA_ORT_DIR}/bin/DirectML.dll\"\n")
    configure_file("${HYDRA_ORT_RC}.in" "${HYDRA_ORT_RC}" COPYONLY)   # rewritten only when it changes
    set_source_files_properties("${HYDRA_ORT_RC}" PROPERTIES
        OBJECT_DEPENDS "${HYDRA_ORT_DIR}/bin/onnxruntime.dll;${HYDRA_ORT_DIR}/bin/DirectML.dll")
else()
    set(HYDRA_STEMS 0)
    message(STATUS "Stems: no ONNX Runtime in external/onnxruntime (run tools/fetch-onnxruntime.ps1) - effect compiled out")
endif()
function(hydra_use_stems target)
    target_compile_definitions(${target} PRIVATE HYDRA_STEMS=${HYDRA_STEMS})
    if(HYDRA_STEMS)
        target_include_directories(${target} PRIVATE "${HYDRA_ORT_DIR}/include")
        # The DLLs are embedded in the exe (RCDATA 201 / 202): a release is always a single exe (user, 10.11.).
        # stems::extractRuntime unpacks them to %APPDATA%\HydraMixer\Runtime on first use.
        target_sources(${target} PRIVATE "${HYDRA_ORT_RC}")
    endif()
endfunction()

# Hydra Remote: the web app for phones / tablets, compiled into the exe (src/remote/RemoteControl.cpp serves it).
juce_add_binary_data(HydraRemoteData HEADER_NAME RemoteData.h NAMESPACE RemoteData
    SOURCES resources/remote/index.html resources/remote/manifest.webmanifest resources/icon.png)

# JUCE's resources.rc (file version shown in Explorer) depends only on the icon, not on the version: after a
# version bump the exe kept the old one (0.8.1 built as 0.8.0). Removed at every configure, it is generated again.
file(REMOVE "${CMAKE_CURRENT_BINARY_DIR}/HydraMixer_artefacts/JuceLibraryCode/HydraMixer_resources.rc")
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
# src/linkcore is the phone app's network core (mobile/android); here only in the tests (LinkLiteTest).
list(FILTER HYDRA_SOURCES EXCLUDE REGEX "/src/linkcore/")
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
        rnnoise opus avrt ole32 uuid mmdevapi ws2_32 iphlpapi bcrypt crypt32 HydraRemoteData
    PUBLIC
        juce::juce_recommended_config_flags
        $<$<NOT:$<BOOL:${HYDRA_FAST}>>:juce::juce_recommended_lto_flags>)
hydra_precompile_headers(HydraMixer)
hydra_use_stems(HydraMixer)

if(MSVC)
    # Realtime audio: aggressive optimisation, AVX2 vectorisation, fast float maths.
    target_compile_options(HydraMixer PRIVATE
        /W3 /permissive- /Zc:__cplusplus /MP
        $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Oi /Ot /arch:AVX2 /fp:fast>)
    if(HYDRA_FAST)
        target_link_options(HydraMixer PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/INCREMENTAL /OPT:NOREF /OPT:NOICF>)
    else()
        target_compile_options(HydraMixer PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/GL>)
        target_link_options(HydraMixer PRIVATE $<$<CONFIG:Release,RelWithDebInfo>:/LTCG /OPT:REF /OPT:ICF>)
    endif()
endif()

# Offline tests (local only, not in the repository): built when tests/ exists.
if(EXISTS "${CMAKE_SOURCE_DIR}/tests")
# Offline DSP tests (run: build\<cfg>\HydraTests_artefacts\<cfg>\HydraTests.exe)
juce_add_console_app(HydraTests PRODUCT_NAME "HydraTests")
target_sources(HydraTests PRIVATE tests/DenoiserTest.cpp tests/SessionTest.cpp tests/MidiRangeTest.cpp tests/AppCaptureTest.cpp tests/LedTest.cpp tests/RecorderTest.cpp tests/AsioSecondaryTest.cpp tests/UndoTest.cpp tests/ReorderTest.cpp tests/SceneTest.cpp tests/SyncDelayTest.cpp tests/RoomAnalysisTest.cpp tests/CorrectionTest.cpp tests/DeviceNamesTest.cpp tests/FxChainTest.cpp tests/CorrectionPoolTest.cpp tests/VacTest.cpp tests/VacStreamTest.cpp tests/NetLinkTest.cpp tests/RemoteTest.cpp tests/LinkLiteTest.cpp tests/StemsTest.cpp
    src/core/StemDsp.cpp src/engine/Stems.cpp src/core/Denoiser.cpp src/core/RoomAnalysis.cpp src/core/Correction.cpp src/engine/Engine.cpp src/engine/IoStreams.cpp src/engine/VacStream.cpp src/engine/NetLink.cpp src/core/NetCodec.cpp src/core/NetCrypto.cpp src/remote/QrCode.cpp src/remote/RemoteServer.cpp src/remote/RemoteControl.cpp src/linkcore/LinkLite.cpp src/engine/AsioSecondary.cpp src/engine/Recorder.cpp src/engine/PluginChain.cpp
    src/midi/MidiMapper.cpp src/midi/Hotkeys.cpp src/state/Session.cpp src/state/VoicemeeterImport.cpp src/state/UndoHistory.cpp src/state/Scenes.cpp src/state/RoomMeasurements.cpp src/state/DeviceNames.cpp src/state/ChannelClipboard.cpp src/state/CorrectionPool.cpp src/state/VacControl.cpp src/state/VacCables.cpp)
target_include_directories(HydraTests PRIVATE "${CMAKE_SOURCE_DIR}/src")
target_compile_definitions(HydraTests PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_definitions(HydraTests PRIVATE JUCE_ASIO=1 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0)
target_link_libraries(HydraTests PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_gui_extra juce::juce_dsp rnnoise opus avrt ole32 uuid mmdevapi ws2_32 iphlpapi bcrypt crypt32 HydraRemoteData
                      PUBLIC juce::juce_recommended_config_flags)
hydra_precompile_headers(HydraTests)
hydra_use_stems(HydraTests)
if(HYDRA_STEMS)   # StemsTest compares the DLLs unpacked from the exe with these
    set_property(SOURCE tests/StemsTest.cpp APPEND PROPERTY COMPILE_DEFINITIONS "HYDRA_ORT_BIN=\"${HYDRA_ORT_DIR}/bin\"")
endif()
# The tests keep several Engine objects on the stack (several 100 KB each since the EQ has 16 bands): 16 MB stack.
target_link_options(HydraTests PRIVATE /STACK:16777216)
endif()

# Screenshot tool (local only, website screenshots): renders the UI with an example layout offscreen. Not part of the release.
# Configure with -DHYDRA_SHOTS=ON, run: HydraShots.exe <output folder>
option(HYDRA_SHOTS "Build the screenshot tool" OFF)
if(HYDRA_SHOTS AND EXISTS "${CMAKE_SOURCE_DIR}/tools/Shots.cpp")
    juce_add_console_app(HydraShots PRODUCT_NAME "HydraShots")
    set(SHOT_SOURCES ${HYDRA_SOURCES})
    list(FILTER SHOT_SOURCES EXCLUDE REGEX "/src/Main\.cpp$")
    target_sources(HydraShots PRIVATE ${SHOT_SOURCES} tools/Shots.cpp)
    target_include_directories(HydraShots PRIVATE "${CMAKE_SOURCE_DIR}/src")
    target_compile_definitions(HydraShots PRIVATE JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_MODAL_LOOPS_PERMITTED=1 NOMINMAX WIN32_LEAN_AND_MEAN
        JUCE_ASIO=1 JUCE_WASAPI=1 JUCE_DIRECTSOUND=0 JUCE_PLUGINHOST_VST3=1 JUCE_PLUGINHOST_VST=0 JUCE_PLUGINHOST_LV2=0 JUCE_PLUGINHOST_LADSPA=0
        JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
    target_link_libraries(HydraShots PRIVATE juce::juce_audio_utils juce::juce_audio_processors juce::juce_audio_devices juce::juce_gui_extra juce::juce_dsp
        rnnoise opus avrt ole32 uuid mmdevapi ws2_32 iphlpapi bcrypt crypt32 HydraRemoteData PUBLIC juce::juce_recommended_config_flags)
    hydra_use_stems(HydraShots)
    target_compile_options(HydraShots PRIVATE /W3 /permissive- /Zc:__cplusplus /MP /O2 /arch:AVX2 /fp:fast)
    target_link_options(HydraShots PRIVATE /STACK:16777216)
endif()
endif()  # NOT HYDRA_PLUGIN_ONLY

# Room Correction as a VST3 plugin (for a DAW's master bus). Shares the corrections and measurements with the mixer
# (%APPDATA%\HydraMixer\Corrections, \Measurements). Build output: build\<cfg>\HydraRoomCorrection_artefacts\<cfg>\VST3
# JUCE's VST3 post-build step runs through cmd and breaks on an "&" in the build folder's path ("Phoebe & Til"):
# then build.ps1 builds the plugin in its own folder (HYDRA_PLUGIN_ONLY) and copies it here.
string(FIND "${CMAKE_BINARY_DIR}" "&" hydraAmpersand)
if(hydraAmpersand EQUAL -1)
juce_add_plugin(HydraRoomCorrection
    PRODUCT_NAME "Hydra Room Correction"
    COMPANY_NAME "Hydra"
    VERSION ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE Hydr
    PLUGIN_CODE HyRc
    FORMATS VST3
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    VST3_CATEGORIES Fx EQ
    COPY_PLUGIN_AFTER_BUILD FALSE)
target_sources(HydraRoomCorrection PRIVATE
    plugin/RoomCorrectionPlugin.cpp plugin/RoomCorrectionEditor.cpp
    src/core/Correction.cpp src/core/RoomAnalysis.cpp src/state/RoomMeasurements.cpp src/state/CorrectionPool.cpp
    src/ui/Theme.cpp src/ui/ChannelIcons.cpp src/ui/Dialog.cpp src/ui/Controls.cpp src/ui/RoomPages.cpp)
target_include_directories(HydraRoomCorrection PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/plugin")
target_compile_definitions(HydraRoomCorrection PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_MODAL_LOOPS_PERMITTED=1
    NOMINMAX
    WIN32_LEAN_AND_MEAN)
target_link_libraries(HydraRoomCorrection
    PRIVATE
        juce::juce_audio_utils
        juce::juce_audio_processors
        juce::juce_audio_devices
        juce::juce_gui_extra
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags)
hydra_precompile_headers(HydraRoomCorrection)
if(MSVC)
    target_compile_options(HydraRoomCorrection PRIVATE
        /W3 /permissive- /Zc:__cplusplus /MP
        $<$<CONFIG:Release,RelWithDebInfo>:/O2 /Oi /Ot /arch:AVX2 /fp:fast>)
endif()
else()
    message(STATUS "Room Correction plugin: built separately (\"&\" in the build path), see build.ps1")
endif()
