add_library(sp404_core STATIC
    src/PadInfo.cpp
    src/Bank.cpp
    src/SdCard.cpp
    src/WavRlnd.cpp
    src/WavInfo.cpp
    src/Pattern.cpp
)

target_include_directories(sp404_core PUBLIC include)
target_compile_features(sp404_core PUBLIC cxx_std_20)

option(SP404_CORE_BUILD_TESTS "Build sp404_core unit tests" ON)
if(SP404_CORE_BUILD_TESTS)
    add_subdirectory(tests)
endif()
