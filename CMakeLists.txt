cmake_minimum_required(VERSION 3.22)

project(AIVoiceConverter VERSION 0.2.2)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.6
    GIT_SHALLOW TRUE)

# SDK de ARA (Celemony, Apache 2.0). JUCE 8 soporta la version 2.2.0; el repo usa submodulos
# (solo hacen falta ARA_API y ARA_Library). No tiene CMakeLists propio: solo se descarga.
FetchContent_Declare(
    ARA_SDK
    GIT_REPOSITORY https://github.com/Celemony/ARA_SDK.git
    GIT_TAG releases/2.2.0
    GIT_SUBMODULES ARA_API ARA_Library
    SOURCE_SUBDIR no-cmake-here)

FetchContent_MakeAvailable(JUCE ARA_SDK)

# antes de crear cualquier plugin que use ARA
juce_set_ara_sdk_path("${ara_sdk_SOURCE_DIR}")

juce_add_plugin(AIVoiceConverter
    COMPANY_NAME "StemTools"
    PRODUCT_NAME "AI Voice Converter"
    PLUGIN_MANUFACTURER_CODE Stmt
    PLUGIN_CODE Rvcv
    FORMATS VST3
    VST3_CATEGORIES "Fx" "Pitch Shift"
    BUNDLE_ID com.stemtools.aivoiceconverter
    IS_ARA_EFFECT TRUE
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE)

# el motor Python va embebido en el plugin y se escribe en disco al usarlo
juce_add_binary_data(AIVoiceConverterData SOURCES engine/rvc_engine.py)

target_sources(AIVoiceConverter PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/AraSupport.cpp
    Source/EngineRunner.cpp)

target_compile_definitions(AIVoiceConverter PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0)

target_link_libraries(AIVoiceConverter
    PRIVATE
        AIVoiceConverterData
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
