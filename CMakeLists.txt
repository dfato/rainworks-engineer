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
set(VE_ONNX_ROOT "" CACHE PATH "Optional: an extracted ONNX Runtime release zip (has include/ and lib/). Skips the download.")

# ---- Engine + State sources (no GUI modules) ---------------------------------
set(VE_ENGINE_SOURCES
    Source/Engine/CaptureEngine.cpp
    Source/Engine/HostContext.cpp
    Source/Engine/JobQueue.cpp
    Source/Engine/NotePlan.cpp
    Source/Engine/PitchCorrectRenderer.cpp
    Source/Engine/PitchTracker.cpp
    Source/Engine/PlaybackEngine.cpp
    Source/Engine/RenderCache.cpp
    Source/Engine/SidechainAnalysis.cpp
    Source/Engine/TakeImporter.cpp
    Source/Engine/Style/BuiltinStyles.cpp
    Source/Engine/Style/ExpressionLayer.cpp
    Source/Engine/Style/StyleProfile.cpp
    Source/Engine/Intent/NoteIntent.cpp
    Source/Engine/Voice/VoiceConverter.cpp
    Source/Engine/Voice/VoiceModel.cpp
    Source/Engine/Voice/VoiceRenderer.cpp
    Source/Engine/TempoMap.cpp
    Source/Engine/TimeWarp.cpp
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
    # ONNX Runtime comes from its official NuGet package (stable URL and layout):
    #   build/native/include/...           headers
    #   runtimes/<rid>/native/...          libraries
    if(WIN32)
        set(_ort_rid "win-x64")
    elseif(APPLE)
        set(_ort_rid "osx-arm64")
    else()
        set(_ort_rid "linux-x64")
    endif()

    if(VE_ONNX_ROOT)
        set(_ort_inc "${VE_ONNX_ROOT}/include")
        set(_ort_lib "${VE_ONNX_ROOT}/lib")
    else()
        set(_ort_dir "${CMAKE_BINARY_DIR}/_deps/onnxruntime-${VE_ONNX_VERSION}")
        if(NOT EXISTS "${_ort_dir}/build/native/include/onnxruntime_cxx_api.h")
            set(_ort_pkg "${CMAKE_BINARY_DIR}/_deps/onnxruntime-${VE_ONNX_VERSION}.nupkg")
            message(STATUS "Downloading ONNX Runtime ${VE_ONNX_VERSION} (NuGet)")
            file(DOWNLOAD "https://www.nuget.org/api/v2/package/Microsoft.ML.OnnxRuntime/${VE_ONNX_VERSION}"
                 "${_ort_pkg}" STATUS _ort_status TLS_VERIFY ON)
            list(GET _ort_status 0 _ort_code)
            if(NOT _ort_code EQUAL 0)
                message(FATAL_ERROR "ONNX Runtime download failed: ${_ort_status}. Set VE_ONNX_ROOT to a local copy instead.")
            endif()
            file(ARCHIVE_EXTRACT INPUT "${_ort_pkg}" DESTINATION "${_ort_dir}")
        endif()
        set(_ort_inc "${_ort_dir}/build/native/include")
        set(_ort_lib "${_ort_dir}/runtimes/${_ort_rid}/native")
    endif()

    add_library(ve_onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(ve_onnxruntime PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${_ort_inc}")
    if(WIN32)
        set_target_properties(ve_onnxruntime PROPERTIES
            IMPORTED_IMPLIB   "${_ort_lib}/onnxruntime.lib"
            IMPORTED_LOCATION "${_ort_lib}/onnxruntime.dll")
    elseif(APPLE)
        set_target_properties(ve_onnxruntime PROPERTIES IMPORTED_LOCATION "${_ort_lib}/libonnxruntime.dylib")
    else()
        set_target_properties(ve_onnxruntime PROPERTIES IMPORTED_LOCATION "${_ort_lib}/libonnxruntime.so")
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

target_include_directories(VocalEngine PRIVATE Source)

# Stage 5 voice models run in-process through ONNX Runtime when VE_WITH_ONNX=ON.
# On Windows the runtime is delay-loaded and shipped next to the plugin binary
# (inside the .vst3 bundle), and the engine loads it from there by full path, so a
# missing or foreign onnxruntime.dll can never crash the host.
if(VE_WITH_ONNX)
    target_compile_definitions(VocalEngine PRIVATE VE_WITH_ONNX=1)
    target_link_libraries(VocalEngine PRIVATE ve_onnxruntime)
    foreach(_fmt VST3 Standalone)
        if(TARGET VocalEngine_${_fmt})
            if(MSVC)
                target_link_options(VocalEngine_${_fmt} PRIVATE "/DELAYLOAD:onnxruntime.dll")
                target_link_libraries(VocalEngine_${_fmt} PRIVATE delayimp)
            endif()
            add_custom_command(TARGET VocalEngine_${_fmt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ve_onnxruntime> $<TARGET_FILE_DIR:VocalEngine_${_fmt}>)
        endif()
    endforeach()
else()
    target_compile_definitions(VocalEngine PRIVATE VE_WITH_ONNX=0)
endif()

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
        add_custom_command(TARGET VocalEngineTests POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different $<TARGET_FILE:ve_onnxruntime> $<TARGET_FILE_DIR:VocalEngineTests>)
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
