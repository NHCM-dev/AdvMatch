cmake_minimum_required(VERSION 3.20)
project(AdvMatch VERSION 0.0.1 LANGUAGES C CXX)

include(FetchContent)
include(CTest)
include(GNUInstallDirs)

find_package(JNI QUIET)
option(ADVMATCH_BUILD_JNI "Build the JNI bridge" ${JNI_FOUND})

# Use the vendored headers without importing upstream build flags or tests.
find_package(Threads REQUIRED)
add_library(advmatch_disruptor INTERFACE)
add_library(AdvMatch::disruptor ALIAS advmatch_disruptor)
target_include_directories(advmatch_disruptor SYSTEM INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/disruptor-cpp/include")
target_link_libraries(advmatch_disruptor INTERFACE Threads::Threads)
target_compile_features(advmatch_disruptor INTERFACE cxx_std_20)

find_package(fmt 11.2 QUIET CONFIG)
if(NOT fmt_FOUND)
    set(FMT_INSTALL OFF CACHE BOOL "Do not install vendored fmt" FORCE)
    FetchContent_Declare(fmt
        GIT_REPOSITORY https://github.com/fmtlib/fmt.git
        GIT_TAG 40626af88bd7df9a5fb80be7b25ac85b122d6c21)
    FetchContent_MakeAvailable(fmt)
endif()

find_package(spdlog 1.15 QUIET CONFIG)
if(NOT spdlog_FOUND)
    set(SPDLOG_FMT_EXTERNAL ON CACHE BOOL "Use the selected fmt package" FORCE)
    FetchContent_Declare(spdlog
        GIT_REPOSITORY https://github.com/gabime/spdlog.git
        GIT_TAG 6fa36017cfd5731d617e1a934f0e5ea9c4445b13)
    FetchContent_MakeAvailable(spdlog)
endif()

add_library(advmatch_core STATIC
    src/advmatch/core.cpp
    include/advmatch/core.hpp)
add_library(AdvMatch::core ALIAS advmatch_core)
set_target_properties(advmatch_core PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(advmatch_core PUBLIC cxx_std_20)
target_compile_definitions(advmatch_core PRIVATE ADVMATCH_VERSION="${PROJECT_VERSION}")
target_include_directories(advmatch_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(advmatch_core PUBLIC AdvMatch::disruptor PRIVATE fmt::fmt spdlog::spdlog)

# Stable C symbols and plain C data types provide the boundary for JNI/PInvoke.
add_library(advmatch SHARED
    src/advmatch/c_api.cpp
    include/advmatch/advmatch.h)
target_compile_features(advmatch PRIVATE cxx_std_20)
target_link_libraries(advmatch PRIVATE AdvMatch::core)
target_include_directories(advmatch PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
set_target_properties(advmatch PROPERTIES OUTPUT_NAME advmatch)
if(WIN32)
    set_target_properties(advmatch PROPERTIES PREFIX "")
endif()

if(ADVMATCH_BUILD_JNI)
    find_package(JNI REQUIRED)
    add_library(advmatch_jni SHARED src/advmatch/jni_bridge.cpp)
    target_link_libraries(advmatch_jni PRIVATE advmatch JNI::JNI)
    set_target_properties(advmatch_jni PROPERTIES OUTPUT_NAME advmatch_jni)
    if(WIN32)
        set_target_properties(advmatch_jni PROPERTIES PREFIX "")
    endif()
endif()

if(BUILD_TESTING)
    add_executable(advmatch_smoke tests/smoke.cpp)
    target_compile_definitions(advmatch_smoke PRIVATE ADVMATCH_EXPECTED_VERSION="${PROJECT_VERSION}")
    target_link_libraries(advmatch_smoke PRIVATE AdvMatch::core advmatch)
    add_test(NAME advmatch_smoke COMMAND advmatch_smoke)
endif()

install(TARGETS advmatch LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR} ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/advmatch/advmatch.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/advmatch)
