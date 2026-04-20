cmake_minimum_required(VERSION 3.15)

project(SendSysEx
    VERSION 1.0.0
    DESCRIPTION "Command-line MIDI SysEx sender"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(RTMIDI_WINDOWS_API "winmm" CACHE STRING "RtMidi Windows API backend: winmm or uwp")
    set_property(CACHE RTMIDI_WINDOWS_API PROPERTY STRINGS winmm uwp)
    string(TOLOWER "${RTMIDI_WINDOWS_API}" RTMIDI_WINDOWS_API)
endif()

set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/inc/rtmidi)
set(MIDI_CPP_DIR ${CMAKE_SOURCE_DIR}/lib/MIDI_CPP)

if(NOT EXISTS "${RTMIDI_DIR}/RtMidi.cpp")
    message(FATAL_ERROR
        "RtMidi is missing. Initialize the submodule with: git submodule update --init --recursive"
    )
endif()

add_executable(SendSysEx
    src/SendSysEx.cpp
    src/kmiDevice.cpp
    src/deviceDatabase.cpp
    ${RTMIDI_DIR}/RtMidi.cpp
    ${MIDI_CPP_DIR}/src/MIDI_sysex.cpp
    ${MIDI_CPP_DIR}/src/MIDI_bytestream_parser.cpp
    ${MIDI_CPP_DIR}/src/MIDI_device_metadata.cpp
)

target_include_directories(SendSysEx PRIVATE
    ${CMAKE_SOURCE_DIR}/inc
    ${CMAKE_SOURCE_DIR}/lib/MIDI_CPP/include
    ${CMAKE_SOURCE_DIR}/lib/json/include
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
    if(RTMIDI_WINDOWS_API STREQUAL "winmm")
        target_compile_definitions(SendSysEx PRIVATE __WINDOWS_MM__)
        target_link_libraries(SendSysEx PRIVATE winmm)
    elseif(RTMIDI_WINDOWS_API STREQUAL "uwp")
        set_property(TARGET SendSysEx PROPERTY CXX_STANDARD 17)
        target_compile_definitions(SendSysEx PRIVATE __WINDOWS_UWP__)
        target_link_libraries(SendSysEx PRIVATE windowsapp)
    else()
        message(FATAL_ERROR "Unsupported RTMIDI_WINDOWS_API='${RTMIDI_WINDOWS_API}'. Use 'winmm' or 'uwp'.")
    endif()
endif()

install(TARGETS SendSysEx RUNTIME DESTINATION bin)
