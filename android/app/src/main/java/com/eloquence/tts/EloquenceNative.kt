/*
 * SPDX-License-Identifier: MIT
 *
 * EloquenceNative -- thin Kotlin view of libeloquence_jni.so. Each method maps
 * 1:1 to a Java_com_eloquence_tts_EloquenceNative_* entry point in
 * android/jni/eloquence_jni.c.
 */
package com.eloquence.tts

object EloquenceNative {
    init { System.loadLibrary("eloquence_jni") }

    /**
     * Initialise an engine instance.
     * @param dataDir directory holding eci.ini (cwd for the engine).
     * @param dialect eciLanguageDialect code (e.g. 0x00010000 = US English),
     *                or 0 for the engine default.
     * @return opaque native handle, or 0 on failure.
     */
    external fun nativeInit(dataDir: String, dialect: Int): Long

    /** Set speed/pitch/volume; pass -1 to leave a field unchanged. */
    external fun nativeSetProsody(handle: Long, rate: Int, pitch: Int, volume: Int)

    /** Synthesize one utterance -> 11025 Hz mono S16 PCM, or null on abort. */
    external fun nativeSynthesize(handle: Long, text: String): ShortArray?

    /** Abort the in-flight utterance (called from onStop). */
    external fun nativeStop(handle: Long)

    /** Tear down the engine instance. */
    external fun nativeShutdown(handle: Long)
}
