cmake_minimum_required(VERSION 3.16)

# Standalone build when invoked directly; otherwise inherits parent project.
if(NOT DEFINED PROJECT_NAME)
    project(sd_eloquence
        VERSION 0.1.0
        DESCRIPTION "Speech Dispatcher module for ETI Eloquence (ECI 6.x)"
        LANGUAGES C)
endif()

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# Locate speech-dispatcher via pkg-config. The .pc file exposes a
# `modulebindir` variable that tells us where the daemon looks for module
# binaries on this system (typically
# /usr/lib/speech-dispatcher/speech-dispatcher-modules). Always use that
# instead of guessing -- different distros put it in different places.
find_package(PkgConfig REQUIRED)
pkg_check_modules(SPEECHD REQUIRED speech-dispatcher)

pkg_get_variable(SPEECHD_MODULEBINDIR speech-dispatcher modulebindir)
if(NOT SPEECHD_MODULEBINDIR)
    message(FATAL_ERROR
        "speech-dispatcher's pkg-config doesn't expose modulebindir. "
        "Your speech-dispatcher install is broken or too old.")
endif()
message(STATUS "speech-dispatcher modulebindir: ${SPEECHD_MODULEBINDIR}")

# Headers come from SPEECHD_INCLUDE_DIRS (parent /usr/include) plus the
# speech-dispatcher subdir that contains spd_module_main.h.
find_path(SPEECHD_INCLUDE_DIR
    NAMES speech-dispatcher/spd_module_main.h
    HINTS ${SPEECHD_INCLUDE_DIRS}
    PATHS /usr/include
)

# The module-side helper library that provides main() + protocol loop.
find_library(SPEECHD_MODULE_LIB
    NAMES speechd_module
    HINTS ${SPEECHD_LIBRARY_DIRS}
    PATHS /usr/lib /usr/lib64
)
if(NOT SPEECHD_INCLUDE_DIR OR NOT SPEECHD_MODULE_LIB)
    message(FATAL_ERROR
        "speech-dispatcher development files not found. "
        "Install speech-dispatcher (Arch: pacman -S speech-dispatcher; "
        "Debian: apt install libspeechd-dev).")
endif()

# Optional libsoxr -- enables output resampling (configurable via eloquence.conf).
option(ENABLE_SOXR "Enable libsoxr-based output resampling" ON)
if(ENABLE_SOXR)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SOXR soxr)
    endif()
    if(NOT SOXR_FOUND)
        find_library(SOXR_LIB NAMES soxr PATHS /usr/lib /usr/lib64)
        find_path(SOXR_INCLUDE_DIR NAMES soxr.h PATHS /usr/include)
        if(SOXR_LIB AND SOXR_INCLUDE_DIR)
            set(SOXR_FOUND TRUE)
            set(SOXR_LIBRARIES ${SOXR_LIB})
            set(SOXR_INCLUDE_DIRS ${SOXR_INCLUDE_DIR})
        endif()
    endif()
    if(SOXR_FOUND)
        message(STATUS "libsoxr found -- resampling enabled")
    else()
        message(STATUS "libsoxr not found -- building without resampling support")
        message(STATUS "  (install with: pacman -S libsoxr  /  apt install libsoxr-dev)")
    endif()
endif()

add_executable(sd_eloquence
    src/sd_eloquence.c
    src/eci_runtime.c
)
target_include_directories(sd_eloquence PRIVATE
    src
    ${SPEECHD_INCLUDE_DIR}
    ${SPEECHD_INCLUDE_DIR}/speech-dispatcher
)
target_link_libraries(sd_eloquence PRIVATE
    ${SPEECHD_MODULE_LIB}
    pthread
    dl
)

if(SOXR_FOUND)
    target_compile_definitions(sd_eloquence PRIVATE HAVE_SOXR)
    target_include_directories(sd_eloquence PRIVATE ${SOXR_INCLUDE_DIRS})
    target_link_libraries(sd_eloquence PRIVATE ${SOXR_LIBRARIES})
endif()

# Install paths:
#   binary:  ${SPEECHD_MODULEBINDIR}/sd_eloquence
#            -- comes from speech-dispatcher's own pkg-config so the daemon
#            actually finds it. Typically /usr/lib/speech-dispatcher/speech-dispatcher-modules.
#   config:  ${CMAKE_INSTALL_FULL_SYSCONFDIR}/speech-dispatcher/modules/eloquence.conf
#            -- with prefix=/usr -> /etc/speech-dispatcher/modules.
#               with prefix=/tmp/xx (test installs) -> /tmp/xx/etc/...
#
# Distro packagers use `cmake --install build --prefix=/usr`.
install(TARGETS sd_eloquence
    RUNTIME DESTINATION ${SPEECHD_MODULEBINDIR}
)
install(FILES eloquence.conf
    DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/speech-dispatcher/modules
)

message(STATUS "")
message(STATUS "sd_eloquence ${PROJECT_VERSION}")
message(STATUS "  speech-dispatcher headers:  ${SPEECHD_INCLUDE_DIR}")
message(STATUS "  speech-dispatcher lib:      ${SPEECHD_MODULE_LIB}")
message(STATUS "  module install dir (from")
message(STATUS "    spd's pkg-config):        ${SPEECHD_MODULEBINDIR}")
message(STATUS "  libsoxr resampling:         ${SOXR_FOUND}")
message(STATUS "")
message(STATUS "After build: install with `sudo make install`, then edit")
message(STATUS "/etc/speech-dispatcher/modules/eloquence.conf to set:")
message(STATUS "  EciLibrary       /absolute/path/to/eci.so")
message(STATUS "  EciVoicePath     /absolute/path/to/enu.so   (or other language)")
message(STATUS "")
message(STATUS "speech-dispatcher auto-picks up modules from")
message(STATUS "/usr/lib/speech-dispatcher-modules + matching .conf in")
message(STATUS "/etc/speech-dispatcher/modules. No speechd.conf edit needed.")
message(STATUS "")
