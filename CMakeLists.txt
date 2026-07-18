cmake_minimum_required(VERSION 3.14)

project(sklad
    VERSION 0.1.0
    DESCRIPTION "An embedded LSM-tree key-value storage engine for C++17"
    HOMEPAGE_URL "https://github.com/VirusAid/sklad"
    LANGUAGES CXX)

# --- options --------------------------------------------------------------
option(SKLAD_BUILD_TESTS    "Build the tests"    ON)
option(SKLAD_BUILD_EXAMPLES "Build the examples" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# --- library --------------------------------------------------------------
add_library(sklad
    src/coding.cpp
    src/bloom.cpp
    src/wal.cpp
    src/sstable.cpp
    src/db.cpp)
add_library(sklad::sklad ALIAS sklad)

target_include_directories(sklad
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_compile_features(sklad PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(sklad PRIVATE /W4)
else()
    target_compile_options(sklad PRIVATE -Wall -Wextra -Wpedantic)
endif()

# std::filesystem needs an extra link on some older toolchains.
if(NOT MSVC AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU"
   AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
    target_link_libraries(sklad PUBLIC stdc++fs)
endif()

# --- tests ----------------------------------------------------------------
if(SKLAD_BUILD_TESTS)
    enable_testing()
    add_executable(sklad_tests tests/test_main.cpp)
    target_link_libraries(sklad_tests PRIVATE sklad)
    add_test(NAME sklad_tests COMMAND sklad_tests)
endif()

# --- examples -------------------------------------------------------------
if(SKLAD_BUILD_EXAMPLES)
    add_executable(demo_kv examples/kv_demo.cpp)
    target_link_libraries(demo_kv PRIVATE sklad)
endif()

# --- install --------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS sklad
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
