cmake_minimum_required(VERSION 3.22)

project(SP404Companion VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 9.0.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(JUCE)

# Rubber Band Library (pitch shift / time stretch, see plugin/SampleDsp.cpp). No CMakeLists.txt
# upstream (its own build system is Meson) -- SOURCE_SUBDIR points at a directory with no
# CMakeLists.txt so FetchContent_MakeAvailable only fetches the source without trying
# add_subdirectory(); plugin/CMakeLists.txt compiles single/RubberBandSingle.cpp (the library's
# own officially-supported single-file amalgamation for exactly this kind of embedding) directly.
# Licence: GPLv2-or-later, or a commercial licence from Breakfast Quay -- see
# ${rubberband_SOURCE_DIR}/COPYING. Only relevant if this plugin is ever distributed.
FetchContent_Declare(
    rubberband
    GIT_REPOSITORY https://github.com/breakfastquay/rubberband.git
    GIT_TAG v4.0.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR no-cmake-here
)
FetchContent_MakeAvailable(rubberband)

include(CTest)

add_subdirectory(core)
add_subdirectory(plugin)
