cmake_minimum_required(VERSION 3.15)

project(SendSysEx
    VERSION 1.0.0
    DESCRIPTION "Command-line MIDI SysEx sender"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(WIN32)
    set(RTMIDI_WINDOWS_API "winmm" CACHE STRING "RtMidi Windows API backend: winmm, uwp, or winmidi2")
    set_property(CACHE RTMIDI_WINDOWS_API PROPERTY STRINGS winmm uwp winmidi2)
    string(TOLOWER "${RTMIDI_WINDOWS_API}" RTMIDI_WINDOWS_API)
endif()

set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/inc/rtmidi)
set(MIDI_CPP_DIR ${CMAKE_SOURCE_DIR}/lib/MIDI_CPP)

if(NOT EXISTS "${RTMIDI_DIR}/RtMidi.cpp")
    message(FATAL_ERROR
        "RtMidi is missing. Initialize the submodule with: git submodule update --init --recursive"
    )
endif()

# Map RTMIDI_WINDOWS_API to rtmidi's API options before adding the subdirectory.
# rtmidi's CMakeLists.txt handles all dependency discovery and linking for each backend.
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS    OFF CACHE BOOL "" FORCE)

if(WIN32)
    if(RTMIDI_WINDOWS_API STREQUAL "winmidi2")
        set(RTMIDI_API_WMS   ON  CACHE BOOL "" FORCE)
        set(RTMIDI_API_WINMM OFF CACHE BOOL "" FORCE)
    elseif(RTMIDI_WINDOWS_API STREQUAL "winmm")
        set(RTMIDI_API_WINMM ON  CACHE BOOL "" FORCE)
        set(RTMIDI_API_WMS   OFF CACHE BOOL "" FORCE)
    elseif(RTMIDI_WINDOWS_API STREQUAL "uwp")
        set(RTMIDI_API_WINMM OFF CACHE BOOL "" FORCE)
        set(RTMIDI_API_WMS   OFF CACHE BOOL "" FORCE)
    else()
        message(FATAL_ERROR "Unsupported RTMIDI_WINDOWS_API='${RTMIDI_WINDOWS_API}'. Use 'winmm', 'uwp', or 'winmidi2'.")
    endif()
endif()

add_subdirectory(${RTMIDI_DIR} rtmidi_build)

add_executable(SendSysEx
    src/SendSysEx.cpp
    src/kmiDevice.cpp
    src/deviceDatabase.cpp
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

# All backend dependencies (WinMM, WMS cppwinrt/midi2, CoreMIDI, etc.) are wired
# by rtmidi's own CMakeLists.txt and propagated through the rtmidi target.
target_link_libraries(SendSysEx PRIVATE rtmidi)

# UWP is not yet handled by rtmidi's cmake; keep the explicit wiring here for now.
if(WIN32 AND RTMIDI_WINDOWS_API STREQUAL "uwp")
    set_property(TARGET SendSysEx PROPERTY CXX_STANDARD 17)
    target_compile_definitions(SendSysEx PRIVATE __WINDOWS_UWP__)
    target_link_libraries(SendSysEx PRIVATE windowsapp)
endif()

install(TARGETS SendSysEx RUNTIME DESTINATION bin)
