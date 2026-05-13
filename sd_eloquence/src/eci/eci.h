/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci.h -- ECI 6.x C API mirroring IBM ibmtts-sdk 6.7.4 eci.h.
 *
 * Signatures are byte-for-byte identical to IBM's; full ABI types are
 * declared here as plain enums/structs. The function-pointer table
 * (EciApi) lives in eci/runtime.h because we dlopen the engine at
 * runtime.
 *
 * IBM's BSD-3-Clause notice is reproduced in the root LICENSE file
 * under "Third-party notices" (and verbatim at docs/ibm-sdk/eci.h);
 * this file is an independent reimplementation of the public ABI
 * declarations and is GPLv2 in the sd_eloquence subtree.
 *
 * Apple's eci.dylib mostly follows the IBM ABI but with quirks worth
 * remembering:
 *   - eciSetParam(eciSampleRate, 2) rejects (no 22050 Hz output).
 *   - eciSynchronize is non-blocking; synthesis continues on a worker
 *     thread after the call returns.
 *   - Apple ships "2"-suffixed extensions (eciNewEx2 etc.) that aren't
 *     in the IBM-documented API.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */

#ifndef SD_ELOQUENCE_ECI_H
#define SD_ELOQUENCE_ECI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Primitive types */

#ifndef MOTIF
typedef int Boolean;
#endif

#define ECITrue   1
#define ECIFalse  0

typedef char ECIsystemChar;
typedef const void *ECIInputText;  /* IBM uses void* for wide-char transparency */
typedef void       *ECIHand;

#define NULL_ECI_HAND  0

#define ECI_PRESET_VOICES        8
#define ECI_USER_DEFINED_VOICES  8
#define ECI_VOICE_NAME_LENGTH    30
#define eciPhonemeLength         4

/* eciProgStatus return bits */
#define ECI_NOERROR            0x00000000
#define ECI_SYSTEMERROR        0x00000001
#define ECI_MEMORYERROR        0x00000002
#define ECI_MODULELOADERROR    0x00000004
#define ECI_DELTAERROR         0x00000008
#define ECI_SYNTHERROR         0x00000010
#define ECI_DEVICEERROR        0x00000020
#define ECI_DICTERROR          0x00000040
#define ECI_PARAMETERERROR     0x00000080
#define ECI_SYNTHESIZINGERROR  0x00000100
#define ECI_DEVICEBUSY         0x00000200
#define ECI_SYNTHESISPAUSED    0x00000400
#define ECI_REENTRANTCALL      0x00000800
#define ECI_ROMANIZERERROR     0x00001000
#define ECI_SYNTHESIZING       0x00002000

/* Engine-wide params (eciSetParam / eciGetParam). Gaps at 4, 6, 11 are
 * unused in ECI 6.x and must not be referenced. */
enum ECIParam {
    eciSynthMode               = 0,
    eciInputType               = 1,
    eciTextMode                = 2,
    eciDictionary              = 3,
    eciSampleRate              = 5,
    eciWantPhonemeIndices      = 7,
    eciRealWorldUnits          = 8,
    eciLanguageDialect         = 9,
    eciNumberMode              = 10,
    eciWantWordIndex           = 12,
    eciNumDeviceBlocks         = 13,
    eciSizeDeviceBlocks        = 14,
    eciNumPrerollDeviceBlocks  = 15,
    eciSizePrerollDeviceBlocks = 16,
    eciNumParams               = 17
};

/* Per-voice params (eciSetVoiceParam / eciGetVoiceParam) */
enum ECIVoiceParam {
    eciGender           = 0,    /* 0=male, 1=female */
    eciHeadSize         = 1,
    eciPitchBaseline    = 2,
    eciPitchFluctuation = 3,
    eciRoughness        = 4,
    eciBreathiness      = 5,
    eciSpeed            = 6,    /* IBM range 0..250, default 50 */
    eciVolume           = 7,    /* 0..100, default 50 */
    eciNumVoiceParams   = 8
};

enum ECILanguageDialect {
    NODEFINEDCODESET             = 0x00000000,
    eciGeneralAmericanEnglish    = 0x00010000,
    eciBritishEnglish            = 0x00010001,
    eciCastilianSpanish          = 0x00020000,
    eciMexicanSpanish            = 0x00020001,
    eciStandardFrench            = 0x00030000,
    eciCanadianFrench            = 0x00030001,
    eciStandardGerman            = 0x00040000,
    eciStandardItalian           = 0x00050000,
    eciMandarinChinese           = 0x00060000,
    eciMandarinChineseGB         = eciMandarinChinese,
    eciMandarinChinesePinYin     = 0x00060100,
    eciMandarinChineseUCS        = 0x00060800,
    eciTaiwaneseMandarin         = 0x00060001,
    eciTaiwaneseMandarinBig5     = eciTaiwaneseMandarin,
    eciTaiwaneseMandarinZhuYin   = 0x00060101,
    eciTaiwaneseMandarinPinYin   = 0x00060201,
    eciTaiwaneseMandarinUCS      = 0x00060801,
    eciBrazilianPortuguese       = 0x00070000,
    eciStandardJapanese          = 0x00080000,
    eciStandardJapaneseSJIS      = eciStandardJapanese,
    eciStandardJapaneseUCS       = 0x00080800,
    eciStandardFinnish           = 0x00090000,
    eciStandardKorean            = 0x000A0000,
    eciStandardKoreanUHC         = eciStandardKorean,
    eciStandardKoreanUCS         = 0x000A0800,
    eciStandardCantonese         = 0x000B0000,
    eciStandardCantoneseGB       = eciStandardCantonese,
    eciStandardCantoneseUCS      = 0x000B0800,
    eciHongKongCantonese         = 0x000B0001,
    eciHongKongCantoneseBig5     = eciHongKongCantonese,
    eciHongKongCantoneseUCS      = 0x000B0801,
    eciStandardDutch             = 0x000C0000,
    eciStandardNorwegian         = 0x000D0000,
    eciStandardSwedish           = 0x000E0000,
    eciStandardDanish            = 0x000F0000,
    eciStandardReserved          = 0x00100000,
    eciStandardThai              = 0x00110000,
    eciStandardThaiTIS           = eciStandardThai
};

/* POS tags for dictionary-update APIs */
enum ECIPartOfSpeech {
    eciUndefinedPOS = 0,
    eciFutsuuMeishi = 1,
    eciKoyuuMeishi,
    eciSahenMeishi,
    eciMingCi
};

enum ECIMessage {
    eciWaveformBuffer    = 0,
    eciPhonemeBuffer     = 1,
    eciIndexReply        = 2,
    eciPhonemeIndexReply = 3,
    eciWordIndexReply    = 4,
    eciStringIndexReply  = 5,
    eciAudioIndexReply   = 6,
    eciSynthesisBreak    = 7
};

enum ECICallbackReturn {
    eciDataNotProcessed = 0,
    eciDataProcessed    = 1,
    eciDataAbort        = 2
};

typedef enum ECICallbackReturn (*ECICallback)(ECIHand hEngine,
                                              enum ECIMessage Msg,
                                              long lParam,
                                              void *pData);

/* Phoneme / mouth-shape data */
typedef struct {
    union {
        unsigned char  sz[eciPhonemeLength + 1];
        unsigned short wsz[eciPhonemeLength + 1];
    } phoneme;
    enum ECILanguageDialect eciLanguageDialect;
    unsigned char mouthHeight;
    unsigned char mouthWidth;
    unsigned char mouthUpturn;
    unsigned char jawOpen;
    unsigned char teethUpperVisible;
    unsigned char teethLowerVisible;
    unsigned char tonguePosn;
    unsigned char lipTension;
} ECIMouthData;

typedef struct ECIVoiceAttrib {
    int eciSampleRate;
    enum ECILanguageDialect languageID;
} ECIVoiceAttrib;

#ifdef __cplusplus
}
#endif

#endif /* SD_ELOQUENCE_ECI_H */
