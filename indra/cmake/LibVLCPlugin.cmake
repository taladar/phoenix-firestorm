# -*- cmake -*-
include(Linking)
include(Prebuilt)

include_guard()
add_library( ll::libvlc INTERFACE IMPORTED )

set(LIBVLCPLUGIN ON CACHE BOOL
        "LIBVLCPLUGIN support for the llplugin/llmedia test apps.")

if (LIBVLCPLUGIN)
    if(LINUX)
        find_package(PkgConfig REQUIRED)

        # media_plugin_libvlc.cpp is written against the VLC 3 API; VLC 4
        # reworked events and stopping, so the plugin does not compile against
        # it. Skip the plugin rather than fail the build -- Linux media still
        # plays through the gstreamer plugin.
        pkg_check_modules(libvlc IMPORTED_TARGET "libvlc<4")
        if (NOT libvlc_FOUND)
            message(WARNING "No VLC 3 libvlc found; not building media_plugin_libvlc")
            set(LIBVLCPLUGIN OFF)
            return()
        endif ()
        target_link_libraries( ll::libvlc INTERFACE PkgConfig::libvlc)
        return()
    endif()

    use_prebuilt_binary(vlc-bin)

    find_library(VLC_LIBRARY
        NAMES
        libvlc.lib
        libvlc.dylib
        PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

    find_library(VLCCORE_LIBRARY
        NAMES
        libvlccore.lib
        libvlccore.dylib
        PATHS "${ARCH_PREBUILT_DIRS_RELEASE}" REQUIRED NO_DEFAULT_PATH)

    target_link_libraries(ll::libvlc INTERFACE ${VLC_LIBRARY} ${VLCCORE_LIBRARY})
endif()
