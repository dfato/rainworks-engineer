cmake_minimum_required(VERSION 3.22)

project(VocalEngine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static MSVC runtime so the plugin does not depend on the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        8.0.9
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

option(VE_BUILD_TESTS "Build the headless engine tests" ON)
option(VE_WITH_ONNX   "Build the optional ONNX Runtime inference backend + smoke test" OFF)
set(VE_ONNX_VERSION "1.20.1" CACHE STRING "Prebuilt ONNX Runtime version to download when VE_WITH_ONNX=ON")
set(VE_ONNX_ROOT "" CACHE PATH "Optional: an extracted ONNX Runtime release (has include/ and lib/). Skips the download.")

# ---- Engine + State sources (no GUI modules) ---------------------------------
set(VE_ENGINE_SOURCES
    Source/Engine/CaptureEngine.cpp
    Source/Engine/HostContext.cpp
    Source/Engine/JobQueue.cpp
    Source/Engine/PlaybackEngine.cpp
    Source/Engine/RenderCache.cpp
    Source/Engine/SidechainAnalysis.cpp
    Source/Engine/TakeImporter.cpp
    Source/Engine/TempoMap.cpp
    Source/Engine/ML/OnnxBackend.cpp
    Source/Engine/ML/SmokeTestModel.cpp
    Source/State/SessionModel.cpp
)
set(VE_ENGINE_MODULES
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_dsp
)

# ---- Optional ONNX Runtime (prebuilt from GitHub releases) -------------------
if(VE_WITH_ONNX)
    if(WIN32)
        set(_ort_pkg "onnxruntime-win-x64-${VE_ONNX_VERSION}.zip")
    elseif(APPLE)
        set(_ort_pkg "onnxruntime-osx-universal2-${VE_ONNX_VERSION}.tgz")
    else()
        set(_ort_pkg "onnxruntime-linux-x64-${VE_ONNX_VERSION}.tgz")
    endif()
    if(VE_ONNX_ROOT)
        set(onnxruntime_SOURCE_DIR "${VE_ONNX_ROOT}")
    else()
        FetchContent_Declare(onnxruntime
            URL https://github.com/microsoft/onnxruntime/releases/download/v${VE_ONNX_VERSION}/${_ort_pkg}
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(onnxruntime)
    endif()

    add_library(ve_onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(ve_onnxruntime PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${onnxruntime_SOURCE_DIR}/include")
    if(WIN32)
        set_target_properties(ve_onnxruntime PROPERTIES
            IMPORTED_IMPLIB   "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.lib"
            IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/onnxruntime.dll")
    elseif(APPLE)
        set_target_properties(ve_onnxruntime PROPERTIES IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.dylib")
    else()
        set_target_properties(ve_onnxruntime PROPERTIES IMPORTED_LOCATION "${onnxruntime_SOURCE_DIR}/lib/libonnxruntime.so")
    endif()
endif()

function(ve_configure_engine_target target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_compile_definitions(${target} PRIVATE VE_WITH_ONNX=$<BOOL:${VE_WITH_ONNX}>)
endfunction()

juce_add_plugin(VocalEngine
    COMPANY_NAME                "RainWorks"
    COMPANY_WEBSITE             ""
    PRODUCT_NAME                "Engineer by RainWorks"
    VERSION                     ${PROJECT_VERSION}
    PLUGIN_MANUFACTURER_CODE    Rnwk
    PLUGIN_CODE                 VEng
    FORMATS                     VST3 Standalone
    IS_SYNTH                    FALSE
    NEEDS_MIDI_INPUT            FALSE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    VST3_CATEGORIES             Fx Tools
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

target_sources(VocalEngine PRIVATE
    ${VE_ENGINE_SOURCES}
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    Source/UI/ControlBar.cpp
    Source/UI/HeaderComponent.cpp
    Source/UI/HostInfoComponent.cpp
    Source/UI/TimelineComponent.cpp
)

# The plugin does not link ONNX Runtime yet (Stage 5 decides in-process vs sidecar);
# its ML backend compiles as the "not available" stub.
target_include_directories(VocalEngine PRIVATE Source)
target_compile_definitions(VocalEngine PRIVATE VE_WITH_ONNX=0)

target_compile_definitions(VocalEngine PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(VocalEngine
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

# ---- Headless engine tests (no GUI modules: enforces the Engine/UI split) ----
if(VE_BUILD_TESTS)
    enable_testing()

    juce_add_console_app(VocalEngineTests PRODUCT_NAME "VocalEngineTests")
    target_sources(VocalEngineTests PRIVATE Tests/EngineTests.cpp ${VE_ENGINE_SOURCES})
    ve_configure_engine_target(VocalEngineTests)
    target_compile_definitions(VocalEngineTests PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(VocalEngineTests PRIVATE ${VE_ENGINE_MODULES}
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(VE_WITH_ONNX)
        target_link_libraries(VocalEngineTests PRIVATE ve_onnxruntime)
    endif()
    add_test(NAME engine COMMAND VocalEngineTests)

    if(VE_WITH_ONNX)
        juce_add_console_app(VocalEngineOnnxSmokeTest PRODUCT_NAME "VocalEngineOnnxSmokeTest")
        target_sources(VocalEngineOnnxSmokeTest PRIVATE Tests/OnnxSmokeTest.cpp ${VE_ENGINE_SOURCES})
        ve_configure_engine_target(VocalEngineOnnxSmokeTest)
        target_compile_definitions(VocalEngineOnnxSmokeTest PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
        target_link_libraries(VocalEngineOnnxSmokeTest PRIVATE ${VE_ENGINE_MODULES} ve_onnxruntime
            PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
        # Put the runtime next to the executable so it is found at launch.
        add_custom_command(TARGET VocalEngineOnnxSmokeTest POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ve_onnxruntime> $<TARGET_FILE_DIR:VocalEngineOnnxSmokeTest>)
        add_test(NAME onnx_smoke COMMAND VocalEngineOnnxSmokeTest)
    endif()
endif()
