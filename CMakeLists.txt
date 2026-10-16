cmake_minimum_required(VERSION 3.18)

project(SendSysEx
    VERSION 1.0.0
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

# rtmidi's own CMakeLists.txt builds the WMS backend and its C++/WinRT projection.
set(RTMIDI_BUILD_SHARED_LIBS OFF CACHE BOOL "Build rtmidi as a static library" FORCE)
set(RTMIDI_BUILD_TESTING OFF CACHE BOOL "Skip rtmidi's own test executables" FORCE)

if(WIN32)
    # WinMM always, plus WMS when found; midiBackend.cpp picks one at runtime.
    set(RTMIDI_API_WINMM ON CACHE BOOL "Compile with WinMM support" FORCE)
endif()
include(${CMAKE_SOURCE_DIR}/cmake/WindowsMidiServices.cmake)

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
    src/bootloaderUpgrade.cpp
    src/usbTopology.cpp
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
    target_link_libraries(SendSysEx PRIVATE cfgmgr32) # usbTopology.cpp
endif()
kmi_wms_bundle_runtime(SendSysEx)

install(TARGETS SendSysEx RUNTIME DESTINATION bin)
if(KMI_WMS_RUNTIME_FILES)
    install(FILES ${KMI_WMS_RUNTIME_FILES} DESTINATION bin)
endif()
