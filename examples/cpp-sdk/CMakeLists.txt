cmake_minimum_required(VERSION 3.20)
project(AdvMatchSdkConsumer LANGUAGES CXX)

find_package(AdvMatch CONFIG REQUIRED)
add_executable(advmatch_cpp_sdk_consumer main.cpp)
target_link_libraries(advmatch_cpp_sdk_consumer PRIVATE AdvMatch::sdk)

if(MINGW)
    target_link_options(advmatch_cpp_sdk_consumer PRIVATE -static)
endif()

if(WIN32)
    add_custom_command(TARGET advmatch_cpp_sdk_consumer POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:AdvMatch::sdk> $<TARGET_FILE_DIR:advmatch_cpp_sdk_consumer>)
endif()
