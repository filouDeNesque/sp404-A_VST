include(FetchContent)

FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG v3.15.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(Catch2)

add_executable(sp404_core_tests
    PadInfoTests.cpp
    WavRlndTests.cpp
    WavInfoTests.cpp
    SdCardTests.cpp
    PatternTests.cpp
    PatternPlayerTests.cpp
)

target_link_libraries(sp404_core_tests PRIVATE sp404_core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(sp404_core_tests)
