cmake_minimum_required(VERSION 3.16)
project(apple-eloquence-elf
    VERSION 0.1.0
    DESCRIPTION "Convert Apple's bundled ETI Eloquence Mach-O dylibs to Linux ELF; speech-dispatcher integration."
    LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# ---------------------------------------------------------------------------
# macho2elf -- the Python+LIEF Mach-O -> ELF converter.
# Installed as a system script so it's invocable as `macho2elf <input.dylib>`.
# Distro packagers should declare a dependency on python-lief (Arch),
# python3-lief (Debian/Ubuntu), python-lief (Fedora COPR).
# ---------------------------------------------------------------------------
install(PROGRAMS macho2elf/macho2elf.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME macho2elf)
install(FILES macho2elf/requirements.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/apple-eloquence-elf)

# ---------------------------------------------------------------------------
# sd_eloquence -- native Speech Dispatcher output module.
# Built only when speech-dispatcher pkg-config is available. Optional libsoxr
# enables runtime resampling.
# ---------------------------------------------------------------------------
option(BUILD_SD_MODULE "Build the sd_eloquence speech-dispatcher module" ON)
option(ENABLE_SOXR     "Enable libsoxr-based output resampling in sd_eloquence" ON)

set(SD_MODULE_STATUS "OFF")

if(BUILD_SD_MODULE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SPEECHD speech-dispatcher)

    if(NOT SPEECHD_FOUND)
        set(SD_MODULE_STATUS "skipped (speech-dispatcher not found)")
        message(STATUS
            "speech-dispatcher not found via pkg-config -- skipping sd_eloquence. "
            "Install speech-dispatcher (Arch: pacman -S speech-dispatcher; "
            "Debian: apt install libspeechd-dev).")
    else()
        # speech-dispatcher exposes a `modulebindir` pkg-config variable that
        # tells us where the daemon looks for module binaries on this system
        # (typically /usr/lib/speech-dispatcher/speech-dispatcher-modules).
        # Always use that instead of guessing -- different distros put it in
        # different places.
        pkg_get_variable(SPEECHD_MODULEBINDIR speech-dispatcher modulebindir)
        if(NOT SPEECHD_MODULEBINDIR)
            message(FATAL_ERROR
                "speech-dispatcher's pkg-config doesn't expose modulebindir. "
                "Your speech-dispatcher install is broken or too old.")
        endif()

        find_path(SPEECHD_INCLUDE_DIR
            NAMES speech-dispatcher/spd_module_main.h
            HINTS ${SPEECHD_INCLUDE_DIRS}
            PATHS /usr/include)
        find_library(SPEECHD_MODULE_LIB
            NAMES speechd_module
            HINTS ${SPEECHD_LIBRARY_DIRS}
            PATHS /usr/lib /usr/lib64)
        if(NOT SPEECHD_INCLUDE_DIR OR NOT SPEECHD_MODULE_LIB)
            message(FATAL_ERROR
                "speech-dispatcher development files not found. "
                "Install speech-dispatcher (Arch: pacman -S speech-dispatcher; "
                "Debian: apt install libspeechd-dev).")
        endif()

        # Optional libsoxr -- enables output resampling (configurable via
        # eloquence.conf). Some distros ship it with a pkg-config file,
        # others don't; try both.
        set(SOXR_FOUND FALSE)
        if(ENABLE_SOXR)
            pkg_check_modules(SOXR soxr)
            if(NOT SOXR_FOUND)
                find_library(SOXR_LIB NAMES soxr PATHS /usr/lib /usr/lib64)
                find_path(SOXR_INCLUDE_DIR NAMES soxr.h PATHS /usr/include)
                if(SOXR_LIB AND SOXR_INCLUDE_DIR)
                    set(SOXR_FOUND TRUE)
                    set(SOXR_LIBRARIES ${SOXR_LIB})
                    set(SOXR_INCLUDE_DIRS ${SOXR_INCLUDE_DIR})
                endif()
            endif()
        endif()

        add_executable(sd_eloquence
            sd_eloquence/src/sd_eloquence.c
            sd_eloquence/src/eci_runtime.c)
        target_include_directories(sd_eloquence PRIVATE
            sd_eloquence/src
            ${SPEECHD_INCLUDE_DIR}
            ${SPEECHD_INCLUDE_DIR}/speech-dispatcher)
        target_link_libraries(sd_eloquence PRIVATE
            ${SPEECHD_MODULE_LIB}
            pthread
            dl)
        if(SOXR_FOUND)
            target_compile_definitions(sd_eloquence PRIVATE HAVE_SOXR)
            target_include_directories(sd_eloquence PRIVATE ${SOXR_INCLUDE_DIRS})
            target_link_libraries(sd_eloquence PRIVATE ${SOXR_LIBRARIES})
        endif()

        # Install paths:
        #   binary:  ${SPEECHD_MODULEBINDIR}/sd_eloquence
        #            -- comes from speech-dispatcher's own pkg-config so the
        #            daemon actually finds it.
        #   config:  ${CMAKE_INSTALL_FULL_SYSCONFDIR}/speech-dispatcher/modules/eloquence.conf
        #            -- with prefix=/usr -> /etc/speech-dispatcher/modules.
        install(TARGETS sd_eloquence
            RUNTIME DESTINATION ${SPEECHD_MODULEBINDIR})
        install(FILES sd_eloquence/eloquence.conf
            DESTINATION ${CMAKE_INSTALL_FULL_SYSCONFDIR}/speech-dispatcher/modules)

        if(SOXR_FOUND)
            set(SD_MODULE_STATUS "ON (with libsoxr resampling)")
        else()
            set(SD_MODULE_STATUS "ON (no libsoxr -- resampling disabled)")
        endif()
    endif()
endif()

# ---------------------------------------------------------------------------
# Examples -- smoke-test binaries that drive the ECI C API directly.
# Built by default; not installed (they're demos).
# ---------------------------------------------------------------------------
option(BUILD_EXAMPLES "Build the speak/version example binaries" ON)
if(BUILD_EXAMPLES)
    add_executable(speak   examples/speak.c)
    add_executable(version examples/version.c)
    target_link_libraries(speak   PRIVATE dl)
    target_link_libraries(version PRIVATE dl)
    set_target_properties(speak version PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
endif()

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------
install(FILES README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    FILES_MATCHING PATTERN "*.md")

# ---------------------------------------------------------------------------
# Optional: install the vendored Apple dylibs + prebuilt ELFs.
# OFF by default since they're derivative of Apple's SDK distribution.
# Distro packagers should leave this OFF and provide a separate "data"
# package that fetches a Simulator Runtime locally at install time.
# Source-builds (developer mode) can enable this with
#   cmake -DINSTALL_PREBUILT_DYLIBS=ON
# ---------------------------------------------------------------------------
option(INSTALL_PREBUILT_DYLIBS
    "Install the bundled Apple dylibs + converted ELFs to /opt/eloquence (legally derivative; OFF by default)"
    OFF)

if(INSTALL_PREBUILT_DYLIBS)
    install(DIRECTORY prebuilt/x86_64/
        DESTINATION /opt/eloquence/x86_64
        FILES_MATCHING PATTERN "*.so")
    install(DIRECTORY prebuilt/aarch64/
        DESTINATION /opt/eloquence/aarch64
        FILES_MATCHING PATTERN "*.so")
    install(DIRECTORY vendor/tvOS-18.2/
        DESTINATION /opt/eloquence/vendor/tvOS-18.2
        FILES_MATCHING PATTERN "*.dylib")
endif()

# ---------------------------------------------------------------------------
# Build summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "apple-eloquence-elf ${PROJECT_VERSION} configured")
message(STATUS "  install prefix:          ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  macho2elf:               ${CMAKE_INSTALL_FULL_BINDIR}/macho2elf")
message(STATUS "  sd_eloquence module:     ${SD_MODULE_STATUS}")
if(BUILD_SD_MODULE AND SPEECHD_FOUND)
    message(STATUS "    install dir:           ${SPEECHD_MODULEBINDIR}")
endif()
message(STATUS "  examples (speak/ver):    ${BUILD_EXAMPLES}")
message(STATUS "  install Apple binaries:  ${INSTALL_PREBUILT_DYLIBS}")
message(STATUS "")
