cmake_minimum_required(VERSION 3.18)

project(SendSysEx
    VERSION 0.13.0
    DESCRIPTION "Command-line MIDI SysEx sender"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RTMIDI_DIR ${CMAKE_SOURCE_DIR}/inc/rtmidi)
set(MIDI_CPP_DIR ${CMAKE_SOURCE_DIR}/lib/MIDI_CPP)

if(NOT EXISTS "${RTMIDI_DIR}/RtMidi.cpp")
    message(FATAL_ERROR
        "RtMidi is missing, or is not the Muse-Kinetics WMS fork. Initialize the "
        "submodule with: git submodule update --init --recursive"
    )
endif()

# Build rtmidi via its own CMakeLists.txt (the Muse-Kinetics WMS fork, pinned
# in inc/rtmidi) rather than compiling RtMidi.cpp directly here. That
# CMakeLists.txt already knows how to detect the Windows MIDI Services (WMS)
# SDK and generate its C++/WinRT projection; duplicating that here would just
# drift out of sync with it.
set(RTMIDI_BUILD_SHARED_LIBS OFF CACHE BOOL "Build rtmidi as a static library" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "Skip rtmidi's own test executables" FORCE)

if(WIN32)
    # Compile both backends in; the CLI selects one at runtime (see
    # midiBackend.cpp): WMS if its SDK runtime is installed, WinMM otherwise.
    # This mirrors the SoftStep editors (shared/KMI_MDM/KMI_ports.cpp).
    set(RTMIDI_API_WINMM ON CACHE BOOL "Compile with WinMM support" FORCE)
    set(RTMIDI_API_WMS ON CACHE BOOL "Compile with Windows MIDI Services support" FORCE)
endif()

add_subdirectory(${RTMIDI_DIR} rtmidi-build EXCLUDE_FROM_ALL)

configure_file(
    ${CMAKE_SOURCE_DIR}/inc/version.h.in
    ${CMAKE_BINARY_DIR}/generated/version.h
    @ONLY
)

add_executable(SendSysEx
    src/SendSysEx.cpp
    src/kmiDevice.cpp
    src/deviceDatabase.cpp
    src/midiBackend.cpp
    ${MIDI_CPP_DIR}/src/MIDI_sysex.cpp
    ${MIDI_CPP_DIR}/src/MIDI_bytestream_parser.cpp
    ${MIDI_CPP_DIR}/src/MIDI_device_metadata.cpp
)

target_include_directories(SendSysEx PRIVATE
    ${CMAKE_SOURCE_DIR}/inc
    ${CMAKE_SOURCE_DIR}/lib/MIDI_CPP/include
    ${CMAKE_SOURCE_DIR}/lib/json/include
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(SendSysEx PRIVATE rtmidi)

if(WIN32)
    # RtMidi.h gates its WMS-specific declarations (e.g.
    # isWindowsMidiServicesAvailable()) behind these same macros, so every
    # translation unit that includes it - not just rtmidi's own - needs them.
    target_compile_definitions(SendSysEx PRIVATE __WINDOWS_MM__ __WINDOWS_MIDI_SERVICES__)
endif()

install(TARGETS SendSysEx RUNTIME DESTINATION bin)
