cmake_minimum_required(VERSION 3.16)
project(apple-eloquence-elf
    VERSION 0.1.0
    DESCRIPTION "Convert Apple's bundled ETI Eloquence Mach-O dylibs to Linux ELF; speech-dispatcher integration."
    LANGUAGES C)

include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# macho2elf -- the Python+LIEF Mach-O -> ELF converter.
# Installed as a system script so it's invocable as `macho2elf <input.dylib>`.
# ---------------------------------------------------------------------------

# Install the converter script with a #!/usr/bin/env python3 shebang.
# Distro packagers should declare a dependency on python-lief (Arch),
# python3-lief (Debian/Ubuntu), python-lief (Fedora COPR).
install(PROGRAMS macho2elf/macho2elf.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME macho2elf
)

# Reference: list the Python dependencies we expect.
install(FILES macho2elf/requirements.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/apple-eloquence-elf
)

# ---------------------------------------------------------------------------
# sd_eloquence -- native Speech Dispatcher output module.
# Built only if speech-dispatcher headers are present; harmless to skip.
# ---------------------------------------------------------------------------
option(BUILD_SD_MODULE "Build the sd_eloquence speech-dispatcher module" ON)

if(BUILD_SD_MODULE)
    find_path(SPEECHD_INCLUDE_DIR
        NAMES speech-dispatcher/spd_module_main.h
        PATHS /usr/include
    )
    if(SPEECHD_INCLUDE_DIR)
        add_subdirectory(sd_eloquence)
    else()
        message(STATUS
            "speech-dispatcher headers not found -- skipping sd_eloquence. "
            "Install speech-dispatcher to enable.")
    endif()
endif()

# ---------------------------------------------------------------------------
# Documentation
# ---------------------------------------------------------------------------
install(FILES README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
)
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    FILES_MATCHING PATTERN "*.md"
)

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
        FILES_MATCHING PATTERN "*.so"
    )
    install(DIRECTORY prebuilt/aarch64/
        DESTINATION /opt/eloquence/aarch64
        FILES_MATCHING PATTERN "*.so"
    )
    install(DIRECTORY vendor/tvOS-18.2/
        DESTINATION /opt/eloquence/vendor/tvOS-18.2
        FILES_MATCHING PATTERN "*.dylib"
    )
endif()

# ---------------------------------------------------------------------------
# Build summary
# ---------------------------------------------------------------------------
message(STATUS "")
message(STATUS "apple-eloquence-elf ${PROJECT_VERSION} configured")
message(STATUS "  install prefix:          ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  macho2elf:               ${CMAKE_INSTALL_FULL_BINDIR}/macho2elf")
message(STATUS "  sd_eloquence module:     ${BUILD_SD_MODULE} (headers: ${SPEECHD_INCLUDE_DIR})")
message(STATUS "  install Apple binaries:  ${INSTALL_PREBUILT_DYLIBS}")
message(STATUS "")
