# Turns RTMIDI_API_WMS on when Windows.Devices.Midi2 is found (WindowsMidiServices_ROOT
# or the Windows SDK); include before add_subdirectory() of RtMidi. See README.md.
# kmi_wms_bundle_runtime(<target>) copies the package's DLL and .pri next to <target>;
# KMI_WMS_RUNTIME_FILES lists them for install().

function(kmi_wms_bundle_runtime target)
    if(KMI_WMS_RUNTIME_FILES)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different ${KMI_WMS_RUNTIME_FILES} "$<TARGET_FILE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

set(KMI_WMS_RUNTIME_FILES "" CACHE INTERNAL "")

if(NOT WIN32)
    return()
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(STATUS "Windows MIDI Services: no 32-bit build exists; WinMM only")
    set(RTMIDI_API_WMS OFF CACHE BOOL "Compile with Windows MIDI Services support" FORCE)
    return()
endif()

# Forget a cached path to metadata that is gone (e.g. the out-of-band SDK's,
# removed when moving to the in-box preview), so it is searched for again.
if(WindowsMidiServices_WINMD AND NOT EXISTS "${WindowsMidiServices_WINMD}")
    unset(WindowsMidiServices_WINMD CACHE)
endif()

list(APPEND CMAKE_MODULE_PATH "${RTMIDI_DIR}/cmake")
find_package(CppWinRT QUIET)
find_package(WindowsMidiServices QUIET)

if(NOT WindowsMidiServices_FOUND)
    message(STATUS "Windows MIDI Services: Windows.Devices.Midi2 not found; WinMM only. "
                   "Set WindowsMidiServices_ROOT to the unpacked Windows.Devices.Midi2 NuGet package to add it.")
    set(RTMIDI_API_WMS OFF CACHE BOOL "Compile with Windows MIDI Services support" FORCE)
    return()
endif()

set(RTMIDI_API_WMS ON CACHE BOOL "Compile with Windows MIDI Services support" FORCE)

# Built against the package: its runtime ships next to the exe.
if(WindowsMidiServices_WINMD)
    get_filename_component(_kmi_wms_root "${WindowsMidiServices_WINMD}/../../.." ABSOLUTE)
    if(CMAKE_GENERATOR_PLATFORM MATCHES "^[Aa][Rr][Mm]64$" OR CMAKE_SYSTEM_PROCESSOR MATCHES "^(ARM64|arm64|aarch64)$")
        set(_kmi_wms_rid win-arm64)
    else()
        set(_kmi_wms_rid win-x64)
    endif()
    set(_kmi_wms_dir "${_kmi_wms_root}/runtimes/${_kmi_wms_rid}/native")
    if(EXISTS "${_kmi_wms_dir}/Windows.Devices.Midi2.dll")
        set(KMI_WMS_RUNTIME_FILES
            "${_kmi_wms_dir}/Windows.Devices.Midi2.dll"
            "${_kmi_wms_dir}/Windows.Devices.Midi2.pri"
            CACHE INTERNAL "")
        message(STATUS "Windows MIDI Services: bundling ${_kmi_wms_dir}")
    else()
        message(WARNING "Windows MIDI Services: no ${_kmi_wms_rid} runtime in ${_kmi_wms_root}; "
                        "the exe will use WinMM unless Windows includes the API.")
    endif()
else()
    message(STATUS "Windows MIDI Services: using the Windows SDK's API; nothing to bundle")
endif()
