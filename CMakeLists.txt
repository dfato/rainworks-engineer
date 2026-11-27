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
option(VE_BUILD_TOOLS "Build command-line tools (style learning)" ON)
option(VE_WITH_ONNX   "Build the optional ONNX Runtime inference backend + smoke test" OFF)
set(VE_ONNX_VERSION "1.20.1" CACHE STRING "Prebuilt ONNX Runtime version to download when VE_WITH_ONNX=ON")
set(VE_ONNX_ROOT "" CACHE PATH "Optional: an extracted ONNX Runtime release zip (has include/ and lib/). Skips the download.")
option(VE_ONNX_DIRECTML "Windows: use the DirectML build of ONNX Runtime so voice models can run on the GPU" ON)
set(VE_DIRECTML_VERSION "1.15.4" CACHE STRING "Microsoft.AI.DirectML package version (DirectML.dll shipped next to the plugin)")
set(VE_PITCH_MODEL_FILE "" CACHE FILEPATH "Optional: pitch-fcpe.onnx (tools/pitch-models/export_fcpe.py) to ship next to the plugin binary")

# ---- Engine + State sources (no GUI modules) ---------------------------------
set(VE_ENGINE_SOURCES
    Source/Engine/CaptureEngine.cpp
    Source/Engine/HostContext.cpp
    Source/Engine/JobQueue.cpp
    Source/Engine/NotePlan.cpp
    Source/Engine/NoteHistory.cpp
    Source/Engine/PitchCorrectRenderer.cpp
    Source/Engine/PitchTracker.cpp
    Source/Engine/PitchTrackerBest.cpp
    Source/Engine/PlaybackEngine.cpp
    Source/Engine/RenderCache.cpp
    Source/Engine/SidechainAnalysis.cpp
    Source/Engine/TakeImporter.cpp
    Source/Engine/Style/BuiltinStyles.cpp
    Source/Engine/Style/ExpressionLayer.cpp
    Source/Engine/Style/ToneShaper.cpp
    Source/Engine/Style/StyleLearner.cpp
    Source/Engine/Style/StyleProfile.cpp
    Source/Engine/Intent/NoteIntent.cpp
    Source/Engine/Voice/VoiceConverter.cpp
    Source/Engine/Voice/VoiceModel.cpp
    Source/Engine/Voice/VoiceRenderer.cpp
    Source/Engine/TempoMap.cpp
    Source/Engine/TimeWarp.cpp
    Source/Engine/ML/MelSpectrogram.cpp
    Source/Engine/ML/NeuralPitch.cpp
    Source/Engine/ML/OnnxBackend.cpp
    Source/Engine/ML/SmokeTestModel.cpp
    Source/Engine/ML/TechniqueDetector.cpp
    Source/State/SessionModel.cpp
)
# Small models compiled into every binary (the technique classifier, ~1.5 MB).
# The pitch model (pitch-fcpe.onnx, 43 MB) is a separate file: see tools/pitch-models.
juce_add_binary_data(VE_ModelData
    HEADER_NAME VEModelData.h
    NAMESPACE   VEModels
    SOURCES     Models/technique-classifier.onnx)
set_target_properties(VE_ModelData PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(VE_ENGINE_MODULES
    VE_ModelData
    juce::juce_core
    juce::juce_events
    juce::juce_data_structures
    juce::juce_audio_basics
    juce::juce_audio_formats
    juce::juce_dsp
)

# ---- Optional ONNX Runtime (prebuilt, from NuGet) -----------------------------
# NuGet packages have a stable URL and layout:
#   build/native/include/...           headers
#   runtimes/<rid>/native/...          libraries
function(ve_fetch_nuget name version out_dir)
    set(_dir "${CMAKE_BINARY_DIR}/_deps/${name}-${version}")
    if(NOT EXISTS "${_dir}/.extracted")
        set(_pkg "${CMAKE_BINARY_DIR}/_deps/${name}-${version}.nupkg")
        message(STATUS "Downloading ${name} ${version} (NuGet)")
        file(DOWNLOAD "https://www.nuget.org/api/v2/package/${name}/${version}" "${_pkg}" STATUS _st TLS_VERIFY ON)
        list(GET _st 0 _code)
        if(NOT _code EQUAL 0)
            message(FATAL_ERROR "${name} download failed: ${_st}. Set VE_ONNX_ROOT to a local copy instead.")
        endif()
        file(ARCHIVE_EXTRACT INPUT "${_pkg}" DESTINATION "${_dir}")
        file(WRITE "${_dir}/.extracted" "")
    endif()
    set(${out_dir} "${_dir}" PARENT_SCOPE)
endfunction()

set(VE_RUNTIME_DLLS "")   # files copied next to every binary that uses ONNX Runtime
set(VE_USE_DML OFF)
if(VE_WITH_ONNX)
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
        if(WIN32 AND VE_ONNX_DIRECTML)
            set(VE_USE_DML ON)
            ve_fetch_nuget(Microsoft.ML.OnnxRuntime.DirectML ${VE_ONNX_VERSION} _ort_dir)
            ve_fetch_nuget(Microsoft.AI.DirectML ${VE_DIRECTML_VERSION} _dml_dir)
            list(APPEND VE_RUNTIME_DLLS "${_dml_dir}/bin/x64-win/DirectML.dll")
        else()
            ve_fetch_nuget(Microsoft.ML.OnnxRuntime ${VE_ONNX_VERSION} _ort_dir)
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
        list(APPEND VE_RUNTIME_DLLS "${_ort_lib}/onnxruntime.dll")
    elseif(APPLE)
        set_target_properties(ve_onnxruntime PROPERTIES IMPORTED_LOCATION "${_ort_lib}/libonnxruntime.dylib")
    else()
        set_target_properties(ve_onnxruntime PROPERTIES IMPORTED_LOCATION "${_ort_lib}/libonnxruntime.so")
    endif()
    if(VE_USE_DML)
        set_target_properties(ve_onnxruntime PROPERTIES INTERFACE_COMPILE_DEFINITIONS "VE_ONNX_DML=1")
    endif()
endif()

function(ve_copy_runtime target)
    foreach(_dll ${VE_RUNTIME_DLLS})
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_dll}" $<TARGET_FILE_DIR:${target}>)
    endforeach()
endfunction()

function(ve_configure_engine_target target)
    target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/Source)
    target_compile_definitions(${target} PRIVATE VE_WITH_ONNX=$<BOOL:${VE_WITH_ONNX}>)
endfunction()

# Processor, editor and UI (shared by the plugin and the UI snapshot tool).
set(VE_EDITOR_SOURCES
    Source/Plugin/PluginProcessor.cpp
    Source/Plugin/PluginEditor.cpp
    Source/UI/ControlBar.cpp
    Source/UI/EngineerLookAndFeel.cpp
    Source/UI/HeaderComponent.cpp
    Source/UI/HostInfoComponent.cpp
    Source/UI/TimelineComponent.cpp
)
# Inter (SIL Open Font License 1.1, see Resources/Fonts/OFL.txt): the UI typeface on every OS.
juce_add_binary_data(VE_UiData
    HEADER_NAME VEUiData.h
    NAMESPACE   VEUi
    SOURCES     Resources/Fonts/Inter-Regular.otf Resources/Fonts/Inter-SemiBold.otf)
set_target_properties(VE_UiData PROPERTIES POSITION_INDEPENDENT_CODE ON)

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
    EDITOR_WANTS_KEYBOARD_FOCUS TRUE
    VST3_CATEGORIES             Fx Tools
    COPY_PLUGIN_AFTER_BUILD     FALSE
)

target_sources(VocalEngine PRIVATE
    ${VE_ENGINE_SOURCES}
    ${VE_EDITOR_SOURCES}
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
            ve_copy_runtime(VocalEngine_${_fmt})
            if(VE_PITCH_MODEL_FILE)
                add_custom_command(TARGET VocalEngine_${_fmt} POST_BUILD
                    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${VE_PITCH_MODEL_FILE}" "$<TARGET_FILE_DIR:VocalEngine_${_fmt}>/pitch-fcpe.onnx")
            endif()
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
        VE_ModelData
        VE_UiData
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
        ve_copy_runtime(VocalEngineTests)
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
        ve_copy_runtime(VocalEngineOnnxSmokeTest)
        add_test(NAME onnx_smoke COMMAND VocalEngineOnnxSmokeTest)
    endif()
endif()

# ---- Command-line tools ----
# EngineerStyleLearn: measures how reference vocals are sung and writes a style profile.
# tools/style-learning uses it to tune the built-in genre styles.
if(VE_BUILD_TOOLS)
    juce_add_console_app(EngineerStyleLearn PRODUCT_NAME "EngineerStyleLearn")
    target_sources(EngineerStyleLearn PRIVATE Source/Tools/StyleLearnMain.cpp ${VE_ENGINE_SOURCES})
    ve_configure_engine_target(EngineerStyleLearn)
    target_compile_definitions(EngineerStyleLearn PRIVATE JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0 JUCE_STANDALONE_APPLICATION=1)
    target_link_libraries(EngineerStyleLearn PRIVATE ${VE_ENGINE_MODULES}
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(VE_WITH_ONNX)
        target_link_libraries(EngineerStyleLearn PRIVATE ve_onnxruntime)
        ve_copy_runtime(EngineerStyleLearn)
    endif()
endif()

# ---- UI snapshot tool (off by default) ----
# EngineerUiSnapshot renders the real editor and its menus to PNG for design review.
# Linux: run it under xvfb-run. See Source/Tools/UiSnapshotMain.cpp.
option(VE_BUILD_UI_SNAPSHOT "Build EngineerUiSnapshot (renders the editor to PNG)" OFF)
if(VE_BUILD_UI_SNAPSHOT)
    juce_add_console_app(EngineerUiSnapshot PRODUCT_NAME "EngineerUiSnapshot")
    target_sources(EngineerUiSnapshot PRIVATE Source/Tools/UiSnapshotMain.cpp ${VE_EDITOR_SOURCES} ${VE_ENGINE_SOURCES})
    ve_configure_engine_target(EngineerUiSnapshot)
    target_compile_definitions(EngineerUiSnapshot PRIVATE
        JUCE_MODAL_LOOPS_PERMITTED=1 JUCE_USE_CURL=0 JUCE_WEB_BROWSER=0
        JucePlugin_Name="Engineer by RainWorks" JucePlugin_VersionString="${PROJECT_VERSION}")
    target_link_libraries(EngineerUiSnapshot PRIVATE VE_ModelData VE_UiData juce::juce_audio_utils juce::juce_dsp
        PUBLIC juce::juce_recommended_config_flags juce::juce_recommended_warning_flags)
    if(VE_WITH_ONNX)
        target_compile_definitions(EngineerUiSnapshot PRIVATE VE_WITH_ONNX=1)
        target_link_libraries(EngineerUiSnapshot PRIVATE ve_onnxruntime)
        ve_copy_runtime(EngineerUiSnapshot)
    endif()
endif()
