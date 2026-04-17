cmake_minimum_required(VERSION 3.15)

project(SendSysEx
    VERSION 1.0.0
    DESCRIPTION "Command-line MIDI SysEx sender"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/inc/rtmidi)

add_executable(SendSysEx
    src/SendSysEx.cpp
    ${RTMIDI_DIR}/RtMidi.cpp
)

target_include_directories(SendSysEx PRIVATE
    ${RTMIDI_DIR}
)

if(APPLE)
    target_compile_definitions(SendSysEx PRIVATE __MACOSX_CORE__)
    target_link_libraries(SendSysEx PRIVATE
        "-framework CoreMIDI"
        "-framework CoreAudio"
        "-framework CoreFoundation"
    )
elseif(WIN32)
    target_compile_definitions(SendSysEx PRIVATE __WINDOWS_MM__)
    target_link_libraries(SendSysEx PRIVATE winmm)
endif()

install(TARGETS SendSysEx RUNTIME DESTINATION bin)
