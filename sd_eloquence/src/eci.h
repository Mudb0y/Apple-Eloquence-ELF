/*
 * eci.h -- ECI 6.x C API for IBM Text To Speech.
 *
 * Mirrors the canonical IBM ibmtts-sdk 6.7.4 eci.h
 * (https://sourceforge.net/projects/ibmtts-sdk/) so this module can drop in
 * against any libc-only ECI 6.x runtime that follows the IBM ABI:
 *   - IBM TTS / ViaVoice for Linux
 *   - Speechworks / ETI distributions (LevelStar Icon, etc.)
 *   - voxin's libvoxin
 *   - Apple's bundled ETI Eloquence eci.dylib (after macho2elf conversion)
 *
 * Differences from IBM's header:
 *   - We dlopen() the runtime instead of link-time binding, so the public
 *     surface is exposed as a single function-pointer struct (EciApi). The
 *     individual member signatures are byte-for-byte identical to the IBM
 *     declarations -- only the indirection changes.
 *   - The MOTIF/_MSC_VER/Windows-CE preprocessor branches from IBM's header
 *     are omitted; Linux is the only target.
 *   - Dictionary, filter, and voice-register/unregister APIs from IBM's
 *     header are not included here because sd_eloquence doesn't use them.
 *     Adding them later requires only updating EciApi + eci_runtime.c.
 *
 * Apple-specific quirks worth noting against the IBM docs:
 *   - eciSetParam(eciSampleRate, 2)  -- IBM accepts (= 22050 Hz);
 *     Apple's build rejects with -1.
 *   - eciSynchronize is non-blocking on Apple's build; the engine continues
 *     to produce callbacks on a worker thread after the function returns.
 *     IBM's eciSynchronize blocks per the documentation.
 *   - eciSetIniContent and several "2"-suffixed entry points exist in
 *     Apple's eci.dylib but are not part of the IBM-documented API.
 *
 * The IBM eci.h carries the BSD license reproduced in the project LICENSE.
 */

#ifndef SD_ELOQUENCE_ECI_H
#define SD_ELOQUENCE_ECI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Primitive types (mirrors IBM eci.h lines 40-69)                           */
/* ------------------------------------------------------------------------- */

#ifndef MOTIF
typedef int Boolean;
#endif

#define ECITrue   1
#define ECIFalse  0

typedef char ECIsystemChar;

/* IBM uses void* so callers can pass char* or wide-char buffers transparently.
 * In practice on Linux it's always an 8-bit char buffer. */
typedef const void *ECIInputText;
typedef void       *ECIHand;

#define NULL_ECI_HAND  0

/* ------------------------------------------------------------------------- */
/* Voice and phoneme limits (IBM eci.h lines 75-78)                          */
/* ------------------------------------------------------------------------- */

#define ECI_PRESET_VOICES        8
#define ECI_USER_DEFINED_VOICES  8
#define ECI_VOICE_NAME_LENGTH    30
#define eciPhonemeLength         4

/* ------------------------------------------------------------------------- */
/* Error bits (returned by eciProgStatus; IBM eci.h lines 80-94)             */
/* ------------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------- */
/* Engine-wide parameters (eciSetParam / eciGetParam; IBM lines 106-122)     */
/*                                                                           */
/* The deliberate gaps at slots 4, 6, 11 are documented in IBM's header --   */
/* those parameter numbers are unused in ECI 6.x and must not be referenced. */
/* ------------------------------------------------------------------------- */

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

/* Per-voice parameters (eciSetVoiceParam / eciGetVoiceParam; IBM lines 124-134) */
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

/* ------------------------------------------------------------------------- */
/* Language dialect codes (IBM eci.h lines 163-204)                          */
/* ------------------------------------------------------------------------- */

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

/* Asian part-of-speech tags used by the dictionary-update APIs (IBM 206-212). */
enum ECIPartOfSpeech {
    eciUndefinedPOS = 0,
    eciFutsuuMeishi = 1,
    eciKoyuuMeishi,
    eciSahenMeishi,
    eciMingCi
};

/* ------------------------------------------------------------------------- */
/* Callback machinery (IBM eci.h lines 244-253)                              */
/* ------------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------- */
/* Phoneme / mouth-shape data (IBM eci.h lines 219-238)                      */
/* ------------------------------------------------------------------------- */

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

/* ------------------------------------------------------------------------- */
/* Runtime function table.                                                   */
/*                                                                           */
/* Member signatures mirror the IBM eci.h declarations exactly. Members that */
/* IBM declares but sd_eloquence doesn't need are still loaded so the table  */
/* doubles as a complete IBM-ABI wrapper for any future caller.              */
/* ------------------------------------------------------------------------- */

typedef struct EciApi {
    /* Lifecycle */
    ECIHand   (*New)(void);
    ECIHand   (*NewEx)(enum ECILanguageDialect);
    ECIHand   (*Delete)(ECIHand);
    Boolean   (*Reset)(ECIHand);
    Boolean   (*IsBeingReentered)(ECIHand);
    void      (*Version)(char *pBuffer);

    /* Diagnostics */
    int       (*ProgStatus)(ECIHand);
    void      (*ErrorMessage)(ECIHand, void *buffer);
    void      (*ClearErrors)(ECIHand);
    Boolean   (*TestPhrase)(ECIHand);

    /* Single-shot speak (no separate AddText/Synthesize) */
    Boolean   (*SpeakText)(ECIInputText pText, Boolean bAnnotationsInTextPhrase);
    Boolean   (*SpeakTextEx)(ECIInputText pText, Boolean bAnnotationsInTextPhrase,
                              enum ECILanguageDialect);

    /* Parameters */
    int       (*GetParam)(ECIHand, enum ECIParam);
    int       (*SetParam)(ECIHand, enum ECIParam, int);
    int       (*GetDefaultParam)(enum ECIParam);
    int       (*SetDefaultParam)(enum ECIParam, int);

    /* Voices */
    Boolean   (*CopyVoice)(ECIHand, int iVoiceFrom, int iVoiceTo);
    Boolean   (*GetVoiceName)(ECIHand, int iVoice, void *pBuffer);
    Boolean   (*SetVoiceName)(ECIHand, int iVoice, const void *pBuffer);
    int       (*GetVoiceParam)(ECIHand, int iVoice, enum ECIVoiceParam);
    int       (*SetVoiceParam)(ECIHand, int iVoice, enum ECIVoiceParam, int);

    /* Synthesis queue */
    Boolean   (*AddText)(ECIHand, ECIInputText);
    Boolean   (*InsertIndex)(ECIHand, int);
    Boolean   (*Synthesize)(ECIHand);
    Boolean   (*SynthesizeFile)(ECIHand, const void *pFilename);
    Boolean   (*ClearInput)(ECIHand);
    Boolean   (*GeneratePhonemes)(ECIHand, int iSize, void *pBuffer);
    int       (*GetIndex)(ECIHand);

    /* Playback control */
    Boolean   (*Stop)(ECIHand);
    Boolean   (*Speaking)(ECIHand);
    Boolean   (*Synchronize)(ECIHand);
    Boolean   (*Pause)(ECIHand, Boolean On);

    /* Output routing */
    Boolean   (*SetOutputBuffer)(ECIHand, int iSize, short *psBuffer);
    Boolean   (*SetOutputFilename)(ECIHand, const void *pFilename);
    Boolean   (*SetOutputDevice)(ECIHand, int iDevNum);

    /* Callbacks */
    void      (*RegisterCallback)(ECIHand, ECICallback, void *pData);

    /* Misc */
    int       (*GetAvailableLanguages)(enum ECILanguageDialect *aLanguages,
                                       int *nLanguages);
} EciApi;

/* dlopen the ECI runtime at <eci_so_path>, populate api, return 0 on success.
 * On failure, errmsg (if non-NULL) is filled with a heap-allocated message
 * the caller must free. */
int  eci_runtime_open(const char *eci_so_path, EciApi *api, char **errmsg);
void eci_runtime_close(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_ELOQUENCE_ECI_H */
