cmake_minimum_required(VERSION 3.15)

project(pluginval VERSION 0.2.9)

# If we are using MSVC we want static runtime linkage
if (MSVC)
    set (CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# If we are compiling for macOS we want to target OS versions down to 10.11
if (APPLE)
    set (CMAKE_OSX_DEPLOYMENT_TARGET "10.11" CACHE INTERNAL "")
endif()

# Adds all the module sources so they appear correctly in the IDE
set_property(GLOBAL PROPERTY USE_FOLDERS YES)
option(JUCE_ENABLE_MODULE_SOURCE_GROUPS "Enable Module Source Groups" ON)

add_subdirectory(modules/juce)

if (DEFINED ENV{VST2_SDK_DIR})
    MESSAGE(STATUS "Building with VST2 SDK: $ENV{VST2_SDK_DIR}")
    juce_set_vst2_sdk_path($ENV{VST2_SDK_DIR})
else()
    MESSAGE(STATUS "Not building with VST2")
endif()

juce_add_gui_app(pluginval
    BUNDLE_ID                       com.Tracktion.pluginval
    COMPANY_NAME                    Tracktion
    ICON_BIG                        "${CMAKE_CURRENT_SOURCE_DIR}/Source/binarydata/icon.png"
    HARDENED_RUNTIME_ENABLED        TRUE
    HARDENED_RUNTIME_OPTIONS        com.apple.security.cs.allow-unsigned-executable-memory com.apple.security.cs.disable-library-validation)

juce_generate_juce_header(pluginval)

target_compile_features(pluginval PRIVATE cxx_std_14)

set_target_properties(pluginval PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden)

set(SourceFiles
    Source/CommandLine.h
    Source/CrashHandler.h
    Source/MainComponent.h
    Source/PluginTests.h
    Source/TestUtilities.h
    Source/Validator.h
    Source/CommandLine.cpp
    Source/CrashHandler.cpp
    Source/Main.cpp
    Source/MainComponent.cpp
    Source/PluginTests.cpp
    Source/tests/BasicTests.cpp
    Source/tests/BusTests.cpp
    Source/tests/ParameterFuzzTests.cpp
    Source/TestUtilities.cpp
    Source/Validator.cpp)

target_sources(pluginval PRIVATE ${SourceFiles})
source_group(TREE ${CMAKE_CURRENT_SOURCE_DIR}/Source PREFIX Source FILES ${SourceFiles})

if (DEFINED ENV{VST2_SDK_DIR})
    target_compile_definitions(pluginval PRIVATE
        JUCE_PLUGINHOST_VST=1)
endif()

target_compile_definitions(pluginval PRIVATE
    JUCE_PLUGINHOST_AU=1
    JUCE_PLUGINHOST_LADSPA=1
    JUCE_PLUGINHOST_VST3=1
    JUCE_USE_CURL=0
    JUCE_WEB_BROWSER=0
    JUCER_ENABLE_GPL_MODE=1
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(pluginval PRIVATE
    juce::juce_audio_devices
    juce::juce_audio_processors
    juce::juce_audio_utils
    juce::juce_recommended_warning_flags)
