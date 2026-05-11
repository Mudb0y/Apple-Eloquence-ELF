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

# Locate speech-dispatcher's module headers and library.
find_path(SPEECHD_INCLUDE_DIR
    NAMES speech-dispatcher/spd_module_main.h
    PATHS /usr/include
)
find_library(SPEECHD_MODULE_LIB
    NAMES speechd_module
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

# Install paths follow GNUInstallDirs:
#   binary:  $CMAKE_INSTALL_LIBDIR/speech-dispatcher-modules/sd_eloquence
#            (with prefix=/usr -> /usr/lib/speech-dispatcher-modules)
#   config:  $CMAKE_INSTALL_FULL_SYSCONFDIR/speech-dispatcher/modules/eloquence.conf
#            (with prefix=/usr -> /etc/speech-dispatcher/modules
#             with prefix=/tmp/xx -> /tmp/xx/etc/speech-dispatcher/modules)
#
# Distro packagers use `cmake --install build --prefix=/usr` which gives the
# correct /etc/ path. Test installs with a non-/usr prefix get the config
# under the prefix's etc/ subdir, so unprivileged dry-runs work.
install(TARGETS sd_eloquence
    RUNTIME DESTINATION ${CMAKE_INSTALL_LIBDIR}/speech-dispatcher-modules
)
install(FILES eloquence.conf
    DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/speech-dispatcher/modules
)

message(STATUS "")
message(STATUS "sd_eloquence ${PROJECT_VERSION}")
message(STATUS "  speech-dispatcher headers: ${SPEECHD_INCLUDE_DIR}")
message(STATUS "  speech-dispatcher lib:     ${SPEECHD_MODULE_LIB}")
message(STATUS "  libsoxr resampling:        ${SOXR_FOUND}")
message(STATUS "")
message(STATUS "After build: install with `sudo make install`, then edit")
message(STATUS "/etc/speech-dispatcher/modules/eloquence.conf to set:")
message(STATUS "  EciLibrary       /absolute/path/to/eci.so")
message(STATUS "  EciVoicePath     /absolute/path/to/enu.so   (or other language)")
message(STATUS "")
message(STATUS "Then in /etc/speech-dispatcher/speechd.conf add:")
message(STATUS "  AddModule \"eloquence\" \"sd_eloquence\" \"eloquence.conf\"")
message(STATUS "")
