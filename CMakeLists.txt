cmake_minimum_required(VERSION 3.16)
project(apple-eloquence-elf
    VERSION 0.1.0
    DESCRIPTION "Convert Apple's bundled ETI Eloquence Mach-O dylibs to Linux ELF; speech-dispatcher integration."
    LANGUAGES C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)

# macho2elf -- the Mach-O -> ELF converter (Python; needs python-lief).
install(PROGRAMS macho2elf/macho2elf.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME macho2elf)
install(FILES macho2elf/requirements.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/apple-eloquence-elf)

# sd_eloquence -- speech-dispatcher module.
# Skipped if speech-dispatcher pkg-config isn't available; libsoxr optional.
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

# Examples (smoke tests; not installed).
option(BUILD_EXAMPLES "Build the speak/version example binaries" ON)
if(BUILD_EXAMPLES)
    add_executable(speak   examples/speak.c)
    add_executable(version examples/version.c)
    target_link_libraries(speak   PRIVATE dl)
    target_link_libraries(version PRIVATE dl)
    set_target_properties(speak version PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/examples)
endif()

# Documentation
install(FILES README.md LICENSE
    DESTINATION ${CMAKE_INSTALL_DOCDIR})
install(DIRECTORY docs/
    DESTINATION ${CMAKE_INSTALL_DOCDIR}
    FILES_MATCHING PATTERN "*.md")

# Optional: install the converted ELFs as the sd_eloquence runtime
# data set in ${CMAKE_INSTALL_FULL_LIBDIR}/eloquence. OFF by default
# because the .so files are derivative of Apple's SDK distribution.
option(INSTALL_PREBUILT_DYLIBS
    "Install the prebuilt ELFs + auto-generated eci.ini to ${CMAKE_INSTALL_FULL_LIBDIR}/eloquence/ (legally derivative; OFF by default)"
    OFF)

# Detect host arch so we install the matching prebuilt/ subdir.
if(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "AMD64")
    set(_PREBUILT_ARCH x86_64)
elseif(CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64" OR CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
    set(_PREBUILT_ARCH aarch64)
else()
    set(_PREBUILT_ARCH "")
endif()

if(INSTALL_PREBUILT_DYLIBS)
    if(NOT _PREBUILT_ARCH)
        message(FATAL_ERROR
            "INSTALL_PREBUILT_DYLIBS=ON but no prebuilt set matches host "
            "${CMAKE_SYSTEM_PROCESSOR}. Build from source via macho2elf, "
            "or disable this option.")
    endif()

    set(_ELOQUENCE_DATA_DIR ${CMAKE_INSTALL_FULL_LIBDIR}/eloquence)
    install(DIRECTORY prebuilt/${_PREBUILT_ARCH}/
        DESTINATION ${_ELOQUENCE_DATA_DIR}
        FILES_MATCHING PATTERN "*.so")

    # Generate the multi-section eci.ini at install time so it respects
    # --prefix and DESTDIR (the Path= entries reference the final
    # location; the file itself is written to DESTDIR-prefixed staging).
    install(CODE "
        set(DATA_DIR \"\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/eloquence\")
        set(STAGED \"\$ENV{DESTDIR}\${DATA_DIR}\")
        set(INI \"\${STAGED}/eci.ini\")
        message(STATUS \"Generating \${INI}\")
        file(WRITE \"\${INI}\"
\"# Generated by apple-eloquence-elf install. Each section binds an
# eciLanguageDialect code to a language .so the engine dlopens at runtime.
# CJK entries also list a Path_Rom= romanizer; without it the language
# modules fail eciNewEx with 'No Romanizer Error'.

\")
        # section | primary.so | romanizer.so (empty for non-CJK)
        set(LANG_TRIPLES
            \"1.0|enu.so|\"        \"1.1|eng.so|\"
            \"2.0|esp.so|\"        \"2.1|esm.so|\"
            \"3.0|fra.so|\"        \"3.1|frc.so|\"
            \"4.0|deu.so|\"
            \"5.0|ita.so|\"
            \"6.0|chs.so|chsrom.so\" \"6.1|cht.so|chtrom.so\"
            \"7.0|ptb.so|\"
            \"8.0|jpn.so|jpnrom.so\"
            \"9.0|fin.so|\"
            \"10.0|kor.so|korrom.so\")
        foreach(triple IN LISTS LANG_TRIPLES)
            string(REPLACE \"|\" \";\" parts \"\${triple}\")
            list(GET parts 0 sect)
            list(GET parts 1 so)
            list(GET parts 2 rom)
            file(APPEND \"\${INI}\" \"[\${sect}]\\nPath=\${DATA_DIR}/\${so}\\n\")
            if(NOT rom STREQUAL \"\")
                file(APPEND \"\${INI}\" \"Path_Rom=\${DATA_DIR}/\${rom}\\n\")
            endif()
            file(APPEND \"\${INI}\" \"Version=6.1\\n\\n\")
        endforeach()
    ")

    # Drop a sensible EloquenceDataDir hint into eloquence.conf is handled
    # by sd_eloquence itself (its default is /usr/lib/eloquence, matching
    # this install path); no install-time conf rewrite needed.
endif()

# Optional: also publish the vendored Apple dylibs (.dylib originals) for
# reproducibility / re-conversion. Same legal posture as the prebuilts.
option(INSTALL_APPLE_DYLIBS
    "Also install the unmodified Apple Mach-O dylibs to ${CMAKE_INSTALL_FULL_DATADIR}/apple-eloquence-elf/vendor/"
    OFF)
if(INSTALL_APPLE_DYLIBS)
    install(DIRECTORY vendor/tvOS-18.2/
        DESTINATION ${CMAKE_INSTALL_FULL_DATADIR}/apple-eloquence-elf/vendor/tvOS-18.2
        FILES_MATCHING PATTERN "*.dylib")
endif()

# Build summary
message(STATUS "")
message(STATUS "apple-eloquence-elf ${PROJECT_VERSION} configured")
message(STATUS "  install prefix:          ${CMAKE_INSTALL_PREFIX}")
message(STATUS "  macho2elf:               ${CMAKE_INSTALL_FULL_BINDIR}/macho2elf")
message(STATUS "  sd_eloquence module:     ${SD_MODULE_STATUS}")
if(BUILD_SD_MODULE AND SPEECHD_FOUND)
    message(STATUS "    install dir:           ${SPEECHD_MODULEBINDIR}")
endif()
message(STATUS "  examples (speak/ver):    ${BUILD_EXAMPLES}")
message(STATUS "  install prebuilt ELFs:   ${INSTALL_PREBUILT_DYLIBS}")
if(INSTALL_PREBUILT_DYLIBS)
    message(STATUS "    arch:                  ${_PREBUILT_ARCH}")
    message(STATUS "    data dir:              ${CMAKE_INSTALL_FULL_LIBDIR}/eloquence")
endif()
message(STATUS "  install Apple dylibs:    ${INSTALL_APPLE_DYLIBS}")
message(STATUS "")
