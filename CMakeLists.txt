cmake_minimum_required(VERSION 3.16)
project(apple-eloquence-elf
    VERSION 1.1.0
    DESCRIPTION "Convert Apple's bundled ETI Eloquence Mach-O dylibs to Linux ELF; speech-dispatcher integration."
    LANGUAGES C)

include(GNUInstallDirs)

# Standardize installed paths on "eloquence" regardless of the project name.
# GNUInstallDirs' default DOCDIR is share/doc/${PROJECT_NAME}, which would
# put docs under share/doc/apple-eloquence-elf -- inconsistent with the
# /usr/lib/eloquence + /etc/speech-dispatcher/modules/eloquence.conf paths
# the rest of the install uses. Force-override to match.
set(ELOQUENCE_SHARE_SUBDIR "eloquence")
set(CMAKE_INSTALL_DOCDIR "${CMAKE_INSTALL_DATADIR}/doc/eloquence" CACHE PATH
    "Documentation install dir (overrides the GNUInstallDirs default to match the rest of the install layout)"
    FORCE)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -Wno-unused-parameter)
enable_testing()

# macho2elf -- the Mach-O -> ELF converter (Python; needs python-lief).
install(PROGRAMS macho2elf/macho2elf.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    RENAME macho2elf)
install(FILES macho2elf/requirements.txt
    DESTINATION ${CMAKE_INSTALL_DATADIR}/${ELOQUENCE_SHARE_SUBDIR})

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

        pkg_check_modules(LIBXML2 REQUIRED libxml-2.0)

        # libpcre2-8 -- PCRE2 (Perl-compatible) regex for anti-crash filters.
        # Optional: -DWITH_PCRE2=OFF makes filters into no-ops.
        option(WITH_PCRE2 "Build with libpcre2-8 anti-crash filters" ON)
        set(PCRE2_FOUND FALSE)
        if(WITH_PCRE2)
            pkg_check_modules(PCRE2 libpcre2-8)
            if(NOT PCRE2_FOUND)
                find_library(PCRE2_LIB NAMES pcre2-8 PATHS /usr/lib /usr/lib64)
                find_path(PCRE2_INCLUDE_DIR NAMES pcre2.h PATHS /usr/include)
                if(PCRE2_LIB AND PCRE2_INCLUDE_DIR)
                    set(PCRE2_FOUND TRUE)
                    set(PCRE2_LIBRARIES ${PCRE2_LIB})
                    set(PCRE2_INCLUDE_DIRS ${PCRE2_INCLUDE_DIR})
                endif()
            endif()
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
            sd_eloquence/src/module.c
            sd_eloquence/src/eci/runtime.c
            sd_eloquence/src/eci/voices.c
            sd_eloquence/src/eci/languages.c
            sd_eloquence/src/eci/engine.c
            sd_eloquence/src/config.c
            sd_eloquence/src/audio/resampler.c
            sd_eloquence/src/audio/sink.c
            sd_eloquence/src/synth/marks.c
            sd_eloquence/src/synth/job.c
            sd_eloquence/src/synth/worker.c
            sd_eloquence/src/synth/pause_mode.c
            sd_eloquence/src/ssml/ssml.c
            sd_eloquence/src/filters/filters.c
            sd_eloquence/src/filters/lang_global.c
            sd_eloquence/src/filters/lang_en.c
            sd_eloquence/src/filters/lang_es.c
            sd_eloquence/src/filters/lang_fr.c
            sd_eloquence/src/filters/lang_de.c
            sd_eloquence/src/filters/lang_pt.c
            sd_eloquence/src/cjk_atexit_override.c)
        target_include_directories(sd_eloquence PRIVATE
            sd_eloquence/src
            ${SPEECHD_INCLUDE_DIR}
            ${SPEECHD_INCLUDE_DIR}/speech-dispatcher
            ${LIBXML2_INCLUDE_DIRS})
        target_compile_definitions(sd_eloquence PRIVATE -DPCRE2_CODE_UNIT_WIDTH=8)
        target_link_libraries(sd_eloquence PRIVATE
            ${SPEECHD_MODULE_LIB}
            ${LIBXML2_LIBRARIES}
            pthread
            dl)
        # -rdynamic so the override above intercepts __cxa_atexit calls
        # from chsrom/cht/jpnrom (and ld-linux on their behalf) at dlopen
        # time; without this, libc's symbol wins and our override never fires.
        # $ORIGIN RPATH so the release tarball can bundle libspeechd_module.so.0
        # next to sd_eloquence (Ubuntu noble doesn't package it; we ship our
        # own copy built from upstream speech-dispatcher 0.12.0).
        # --no-as-needed,-lgcc_s pulls libgcc_s into NEEDED so eci.so's
        # _Unwind_Resume resolves at dlopen time. On aarch64 the system
        # libc++ doesn't re-export it; without this the engine fails to
        # load (the binary's own code never references _Unwind_Resume so
        # the linker would otherwise drop libgcc_s).
        target_link_options(sd_eloquence PRIVATE
            -rdynamic
            "-Wl,-rpath,\$ORIGIN"
            "-Wl,--no-as-needed,-lgcc_s,--as-needed")
        if(SOXR_FOUND)
            target_compile_definitions(sd_eloquence PRIVATE HAVE_SOXR)
            target_include_directories(sd_eloquence PRIVATE ${SOXR_INCLUDE_DIRS})
            target_link_libraries(sd_eloquence PRIVATE ${SOXR_LIBRARIES})
        endif()
        if(PCRE2_FOUND)
            target_compile_definitions(sd_eloquence PRIVATE HAVE_PCRE2)
            target_include_directories(sd_eloquence PRIVATE ${PCRE2_INCLUDE_DIRS})
            target_link_libraries(sd_eloquence PRIVATE ${PCRE2_LIBRARIES})
        endif()

        install(TARGETS sd_eloquence
            RUNTIME DESTINATION ${SPEECHD_MODULEBINDIR})

        # Standalone tool that renders demo WAVs through each
        # libsoxr resampler preset. CI invokes it once at release-build
        # time; the resulting share/eloquence/resampler-previews/*.wav
        # ship in the tarball. Not installed.
        if(SOXR_FOUND)
            add_executable(render_resampler_previews
                sd_eloquence/tools/render_resampler_previews.c
                sd_eloquence/src/audio/resampler.c)
            target_compile_definitions(render_resampler_previews PRIVATE HAVE_SOXR)
            target_include_directories(render_resampler_previews PRIVATE
                sd_eloquence/src ${SOXR_INCLUDE_DIRS})
            target_link_libraries(render_resampler_previews PRIVATE
                ${SOXR_LIBRARIES} dl)
            target_link_options(render_resampler_previews PRIVATE
                "-Wl,--no-as-needed,-lgcc_s,--as-needed")
            set_target_properties(render_resampler_previews PROPERTIES
                RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
        endif()

        # Install paths:
        #   binary:  ${SPEECHD_MODULEBINDIR}/sd_eloquence
        #            -- comes from speech-dispatcher's own pkg-config so the
        #            daemon actually finds it.
        #   config:  ${CMAKE_INSTALL_FULL_SYSCONFDIR}/speech-dispatcher/modules/eloquence.conf
        #            -- with prefix=/usr -> /etc/speech-dispatcher/modules.

        # Unit tests for the new sd_eloquence module. These don't need eci.so.
        add_executable(test_voices
            sd_eloquence/tests/test_voices.c
            sd_eloquence/src/eci/voices.c)
        target_include_directories(test_voices PRIVATE sd_eloquence/src)
        add_test(NAME test_voices COMMAND test_voices)

        add_executable(test_languages
            sd_eloquence/tests/test_languages.c
            sd_eloquence/src/eci/languages.c)
        target_include_directories(test_languages PRIVATE sd_eloquence/src)
        add_test(NAME test_languages COMMAND test_languages)

        add_executable(test_config
            sd_eloquence/tests/test_config.c
            sd_eloquence/src/config.c
            sd_eloquence/src/eci/voices.c
            sd_eloquence/src/eci/languages.c)
        target_include_directories(test_config PRIVATE sd_eloquence/src)
        add_test(NAME test_config COMMAND test_config)

        add_executable(test_marks
            sd_eloquence/tests/test_marks.c
            sd_eloquence/src/synth/marks.c)
        target_include_directories(test_marks PRIVATE sd_eloquence/src)
        target_link_libraries(test_marks PRIVATE pthread)
        add_test(NAME test_marks COMMAND test_marks)

        add_executable(test_ssml
            sd_eloquence/tests/test_ssml.c
            sd_eloquence/src/ssml/ssml.c
            sd_eloquence/src/synth/job.c
            sd_eloquence/src/synth/marks.c
            sd_eloquence/src/eci/voices.c
            sd_eloquence/src/eci/languages.c)
        target_include_directories(test_ssml PRIVATE
            sd_eloquence/src ${LIBXML2_INCLUDE_DIRS}
            ${SPEECHD_INCLUDE_DIR}
            ${SPEECHD_INCLUDE_DIR}/speech-dispatcher)
        target_link_libraries(test_ssml PRIVATE ${LIBXML2_LIBRARIES} pthread)
        add_test(NAME test_ssml COMMAND test_ssml)

        add_executable(test_pause_mode
            sd_eloquence/tests/test_pause_mode.c
            sd_eloquence/src/synth/pause_mode.c)
        target_include_directories(test_pause_mode PRIVATE sd_eloquence/src)
        if(PCRE2_FOUND)
            target_compile_definitions(test_pause_mode PRIVATE HAVE_PCRE2 PCRE2_CODE_UNIT_WIDTH=8)
            target_include_directories(test_pause_mode PRIVATE ${PCRE2_INCLUDE_DIRS})
            target_link_libraries(test_pause_mode PRIVATE ${PCRE2_LIBRARIES})
        endif()
        add_test(NAME test_pause_mode COMMAND test_pause_mode)

        add_executable(test_filters
            sd_eloquence/tests/test_filters.c
            sd_eloquence/src/filters/filters.c
            sd_eloquence/src/filters/lang_global.c
            sd_eloquence/src/filters/lang_en.c
            sd_eloquence/src/filters/lang_es.c
            sd_eloquence/src/filters/lang_fr.c
            sd_eloquence/src/filters/lang_de.c
            sd_eloquence/src/filters/lang_pt.c)
        target_include_directories(test_filters PRIVATE sd_eloquence/src)
        if(PCRE2_FOUND)
            target_compile_definitions(test_filters PRIVATE HAVE_PCRE2 PCRE2_CODE_UNIT_WIDTH=8)
            target_include_directories(test_filters PRIVATE ${PCRE2_INCLUDE_DIRS})
            target_link_libraries(test_filters PRIVATE ${PCRE2_LIBRARIES})
        endif()
        add_test(NAME test_filters COMMAND test_filters)

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
    add_executable(mark_probe examples/mark_probe.c)
    # cjk_probe links the same __cxa_atexit override + -rdynamic that
    # sd_eloquence uses, so we can validate the in-module fix end to end
    # without LD_PRELOAD.
    add_executable(cjk_probe
        examples/cjk_probe.c
        sd_eloquence/src/cjk_atexit_override.c)
    target_link_libraries(speak   PRIVATE dl)
    target_link_libraries(version PRIVATE dl)
    target_link_libraries(cjk_probe PRIVATE dl)
    target_link_options(cjk_probe PRIVATE -rdynamic)
    set_target_properties(speak version mark_probe cjk_probe PROPERTIES
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

endif()

# Optional: also publish the vendored Apple dylibs (.dylib originals) for
# reproducibility / re-conversion. Same legal posture as the prebuilts.
option(INSTALL_APPLE_DYLIBS
    "Also install the unmodified Apple Mach-O dylibs to ${CMAKE_INSTALL_FULL_DATADIR}/${ELOQUENCE_SHARE_SUBDIR}/vendor/"
    OFF)
if(INSTALL_APPLE_DYLIBS)
    install(DIRECTORY vendor/tvOS-18.2/
        DESTINATION ${CMAKE_INSTALL_FULL_DATADIR}/${ELOQUENCE_SHARE_SUBDIR}/vendor/tvOS-18.2
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
