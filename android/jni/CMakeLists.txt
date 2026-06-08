# Builds libeloquence_jni.so — the JNI bridge to the converted Eloquence engine.
# Referenced from app/build.gradle.kts via externalNativeBuild.
cmake_minimum_required(VERSION 3.22)
project(eloquence_jni C)

add_library(eloquence_jni SHARED eloquence_jni.c)

# Optional but recommended: interpose __cxa_atexit to suppress the CJK
# romanizers' bad destructor registrations (chsrom/chtrom/jpnrom/korrom), the
# same workaround sd_eloquence uses on desktop Linux. This file is GPL-2.0-or-
# later; including it makes the resulting libeloquence_jni.so GPL-2.0-or-later.
# Comment this out (and the target_sources line) if you only ship non-CJK voices
# and want to keep the JNI lib MIT.
option(ELOQUENCE_CJK_ATEXIT "Bundle the CJK __cxa_atexit override (GPL)" ON)
if(ELOQUENCE_CJK_ATEXIT)
    target_sources(eloquence_jni PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../../sd_eloquence/src/cjk_atexit_override.c)
    target_compile_definitions(eloquence_jni PRIVATE ELOQUENCE_CJK_ATEXIT=1)
endif()

find_library(log-lib log)
target_link_libraries(eloquence_jni ${log-lib})
