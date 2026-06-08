/*
 * SPDX-License-Identifier: MIT
 *
 * EloquenceTtsService -- an Android TextToSpeechService backed by the converted
 * Apple Eloquence engine. Synthesis is delegated to libeloquence_jni.so, which
 * dlopens eci.so + the language modules (shipped in the app's native lib dir)
 * and returns 11025 Hz mono S16 PCM that we hand to the platform callback.
 *
 * Engine data layout at runtime:
 *   - eci.so + lib<lang>.so live in applicationInfo.nativeLibraryDir (installed
 *     read-only from app/src/main/jniLibs/arm64-v8a/, where execution is
 *     allowed -- you cannot dlopen executable code from writable app storage on
 *     modern Android).
 *   - eci.ini is generated into filesDir at first run with absolute Path=
 *     entries pointing into nativeLibraryDir; the engine reads it from cwd, so
 *     the JNI layer chdir()s to filesDir before eciNew.
 */
package com.eloquence.tts

import android.media.AudioFormat
import android.speech.tts.SynthesisCallback
import android.speech.tts.SynthesisRequest
import android.speech.tts.TextToSpeech
import android.speech.tts.TextToSpeechService
import android.util.Log
import java.io.File

private const val TAG = "EloquenceTts"
private const val SAMPLE_RATE = 11025

/** One shipped voice: ISO3 language/country -> ECI dialect + module file(s). */
private data class Voice(
    val iso3Lang: String,
    val iso3Country: String,
    val dialect: Int,
    val module: String,            // lib<module>.so in nativeLibraryDir
    val romanizer: String? = null, // CJK only
)

// Mirrors release.yml's eci.ini table. Modules are installed as lib<name>.so.
private val VOICES = listOf(
    Voice("eng", "USA", 0x00010000, "enu"),
    Voice("eng", "GBR", 0x00010001, "eng"),
    Voice("spa", "ESP", 0x00020000, "esp"),
    Voice("spa", "MEX", 0x00020001, "esm"),
    Voice("fra", "FRA", 0x00030000, "fra"),
    Voice("fra", "CAN", 0x00030001, "frc"),
    Voice("deu", "DEU", 0x00040000, "deu"),
    Voice("ita", "ITA", 0x00050000, "ita"),
    Voice("por", "BRA", 0x00070000, "ptb"),
    Voice("fin", "FIN", 0x00090000, "fin"),
    Voice("zho", "CHN", 0x00060000, "chs", "chsrom"),
    Voice("zho", "TWN", 0x00060001, "cht", "chtrom"),
    Voice("jpn", "JPN", 0x00080000, "jpn", "jpnrom"),
    Voice("kor", "KOR", 0x000A0000, "kor", "korrom"),
)

class EloquenceTtsService : TextToSpeechService() {

    private var handle: Long = 0
    private var currentDialect: Int = 0
    private val lock = Any()

    override fun onCreate() {
        // Engine + eci.ini must exist before the base class wires up languages.
        writeEciIni()
        ensureEngine(VOICES.first().dialect)
        super.onCreate()
    }

    override fun onDestroy() {
        synchronized(lock) {
            if (handle != 0L) { EloquenceNative.nativeShutdown(handle); handle = 0 }
        }
        super.onDestroy()
    }

    // --- language plumbing -------------------------------------------------

    private fun match(lang: String?, country: String?): Voice? {
        if (lang == null) return null
        VOICES.firstOrNull { it.iso3Lang == lang && it.iso3Country == country }?.let { return it }
        return VOICES.firstOrNull { it.iso3Lang == lang } // language-only fallback
    }

    override fun onIsLanguageAvailable(lang: String?, country: String?, variant: String?): Int {
        val v = match(lang, country) ?: return TextToSpeech.LANG_NOT_SUPPORTED
        return when {
            v.iso3Lang == lang && v.iso3Country == country -> TextToSpeech.LANG_COUNTRY_AVAILABLE
            else -> TextToSpeech.LANG_AVAILABLE
        }
    }

    override fun onGetLanguage(): Array<String> {
        val v = VOICES.firstOrNull { it.dialect == currentDialect } ?: VOICES.first()
        return arrayOf(v.iso3Lang, v.iso3Country, "")
    }

    override fun onLoadLanguage(lang: String?, country: String?, variant: String?): Int {
        val v = match(lang, country) ?: return TextToSpeech.LANG_NOT_SUPPORTED
        synchronized(lock) { ensureEngine(v.dialect) }
        return onIsLanguageAvailable(lang, country, variant)
    }

    // --- synthesis ---------------------------------------------------------

    override fun onSynthesizeText(request: SynthesisRequest, callback: SynthesisCallback) {
        val v = match(request.language, request.country)
        if (v == null) { callback.error(); return }

        val text = request.charSequenceText?.toString().orEmpty()
        synchronized(lock) {
            if (!ensureEngine(v.dialect)) { callback.error(); return }
            // Android speechRate/pitch are percent (100 = normal). Map to ECI:
            // eciSpeed 0..250 (≈50 normal); eciPitchBaseline ≈69 normal.
            val rate = (request.speechRate * 50 / 100).coerceIn(0, 250)
            val pitch = (69 * request.pitch / 100).coerceIn(0, 100)
            EloquenceNative.nativeSetProsody(handle, rate, pitch, -1)

            val pcm = EloquenceNative.nativeSynthesize(handle, text)
            if (pcm == null) { callback.error(); return }
            deliver(pcm, callback)
        }
    }

    override fun onStop() {
        synchronized(lock) { if (handle != 0L) EloquenceNative.nativeStop(handle) }
    }

    private fun deliver(pcm: ShortArray, callback: SynthesisCallback) {
        callback.start(SAMPLE_RATE, AudioFormat.ENCODING_PCM_16BIT, 1)
        val maxBytes = callback.maxBufferSize.coerceAtLeast(2)
        val bytes = ByteArray(pcm.size * 2)
        for (i in pcm.indices) {
            bytes[i * 2]     = (pcm[i].toInt() and 0xFF).toByte()
            bytes[i * 2 + 1] = ((pcm[i].toInt() shr 8) and 0xFF).toByte()
        }
        var off = 0
        while (off < bytes.size) {
            val n = minOf(maxBytes, bytes.size - off)
            if (callback.audioAvailable(bytes, off, n) != TextToSpeech.SUCCESS) break
            off += n
        }
        callback.done()
    }

    // --- engine lifecycle --------------------------------------------------

    private fun ensureEngine(dialect: Int): Boolean {
        if (handle != 0L && dialect == currentDialect) return true
        if (handle != 0L) { EloquenceNative.nativeShutdown(handle); handle = 0 }
        handle = EloquenceNative.nativeInit(filesDir.absolutePath, dialect)
        if (handle == 0L) { Log.e(TAG, "nativeInit failed for dialect 0x%08x".format(dialect)); return false }
        currentDialect = dialect
        return true
    }

    private fun writeEciIni() {
        val libDir = applicationInfo.nativeLibraryDir
        val sb = StringBuilder()
        sb.append("# Generated by EloquenceTtsService. Path= entries point at the\n")
        sb.append("# app's read-only native lib dir (execute-allowed).\n\n")
        for (v in VOICES) {
            val hi = (v.dialect ushr 16) and 0xFFFF
            val lo = v.dialect and 0xFFFF
            sb.append("[$hi.$lo]\n")
            sb.append("Path=$libDir/lib${v.module}.so\n")
            v.romanizer?.let { sb.append("Path_Rom=$libDir/lib$it.so\n") }
            sb.append("Version=6.1\n\n")
        }
        File(filesDir, "eci.ini").writeText(sb.toString())
    }
}
