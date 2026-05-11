/*
 * eci.h -- ECI 6.x C API declarations.
 *
 * Source: pyibmtts 0.x SDK from <https://sourceforge.net/projects/ibmtts-sdk/>
 * Cross-checked against the Apple-shipped ETI Eloquence eci.dylib runtime
 * behavior (see apple-eloquence-elf project).
 *
 * Notes on parameter numbering:
 *
 *   The pyibmtts SDK declares ECIParam with deliberate gaps:
 *     eciSynthMode      = 0
 *     eciInputType      = 1
 *     eciTextMode       = 2
 *     eciDictionary     = 3
 *     /\* 4 unused *\/
 *     eciSampleRate     = 5
 *     /\* 6 unused *\/
 *     eciWantPhonemeIndices = 7
 *     ...
 *
 *   Apple's eci.dylib 6.1 follows this numbering exactly. In particular,
 *   eciSetParam(h, 5, 0)  -> 8 kHz output (accepted)
 *   eciSetParam(h, 5, 1)  -> 11025 Hz output (default, accepted)
 *   eciSetParam(h, 5, 2)  -> rejected with -1 (Apple's build doesn't enable 22 kHz).
 *
 *   Other ECI builds (Speechworks 2000-2006 era, IBMTTS for Windows, voxin)
 *   do accept value 2 = 22050 Hz.
 */

#ifndef SD_ELOQUENCE_ECI_H
#define SD_ELOQUENCE_ECI_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque engine handle */
typedef void *ECIHand;

/* Engine-wide parameters (eciSetParam / eciGetParam). Numbering is intentional. */
enum ECIParam {
    eciSynthMode               = 0,   /* 0=screen reader, 1=TTS general text */
    eciInputType               = 1,   /* 0=character, 1=phonetic, 2=TTS */
    eciTextMode                = 2,   /* 0=normal, 1=alphanumeric, 2=verbatim, 3=spell */
    eciDictionary              = 3,   /* 0=use dictionary, 1=skip */
    /* 4 unused */
    eciSampleRate              = 5,   /* 0=8kHz, 1=11025Hz, 2=22050Hz */
    /* 6 unused */
    eciWantPhonemeIndices      = 7,
    eciRealWorldUnits          = 8,
    eciLanguageDialect         = 9,
    eciNumberMode              = 10,
    /* 11 unused */
    eciWantWordIndex           = 12,
    eciNumDeviceBlocks         = 13,
    eciSizeDeviceBlocks        = 14,
    eciNumPrerollDeviceBlocks  = 15,
    eciSizePrerollDeviceBlocks = 16,
    eciNumParams               = 17,
};

/* Per-voice parameters (eciSetVoiceParam / eciGetVoiceParam) */
enum ECIVoiceParam {
    eciGender          = 0,  /* 0=male, 1=female */
    eciHeadSize        = 1,
    eciPitchBaseline   = 2,
    eciPitchFluctuation= 3,
    eciRoughness       = 4,
    eciBreathiness     = 5,
    eciSpeed           = 6,
    eciVolume          = 7,
    eciNumVoiceParams  = 8,
};

/* Voice slots (eciSetVoiceName / eciGetVoiceName: slot 0..7 are presets,
   slot 8..15 user-defined). */
enum {
    ECI_PRESET_VOICES       = 8,
    ECI_USER_DEFINED_VOICES = 8,
    ECI_VOICE_NAME_LENGTH   = 30,
};

/* Language dialect codes (eciSetParam eciLanguageDialect, or eciNewEx). */
enum ECILanguageDialect {
    eciGeneralAmericanEnglish   = 0x00010000,
    eciBritishEnglish           = 0x00010001,
    eciCastilianSpanish         = 0x00020000,
    eciMexicanSpanish           = 0x00020001,
    eciStandardFrench           = 0x00030000,
    eciCanadianFrench           = 0x00030001,
    eciStandardGerman           = 0x00040000,
    eciStandardItalian          = 0x00050000,
    eciMandarinChinese          = 0x00060000,
    eciMandarinChineseGB        = 0x00060000, /* alias for GB encoding */
    eciMandarinChinesePinYin    = 0x00060100,
    eciMandarinChineseUCS       = 0x00060003,
    eciTaiwaneseMandarin        = 0x00060001,
    eciTaiwaneseMandarinBig5    = 0x00060001,
    eciTaiwaneseMandarinZhuYin  = 0x00060101,
    eciTaiwaneseMandarinPinYin  = 0x00060201,
    eciTaiwaneseMandarinUCS     = 0x00060004,
    eciBrazilianPortuguese      = 0x00070000,
    eciStandardJapanese         = 0x00080000,
    eciStandardJapaneseSJIS     = 0x00080000,
    eciStandardJapaneseUCS      = 0x00080002,
    eciStandardFinnish          = 0x00090000,
    eciStandardKorean           = 0x000A0000,
    eciStandardKoreanUHC        = 0x000A0000,
    eciStandardKoreanUCS        = 0x000A0001,
    eciStandardCantonese        = 0x000B0000,
    eciStandardCantoneseGB      = 0x000B0000,
    eciStandardCantoneseUCS     = 0x000B0002,
    eciHongKongCantonese        = 0x000B0001,
    eciHongKongCantoneseBig5    = 0x000B0001,
    eciHongKongCantoneseUCS     = 0x000B0003,
    eciStandardDutch            = 0x000C0000,
    eciStandardNorwegian        = 0x000D0000,
    eciStandardSwedish          = 0x000E0000,
    eciStandardDanish           = 0x000F0000,
    eciStandardReserved         = 0x00100000,
    eciStandardThai             = 0x00110000,
    eciStandardThaiTIS          = 0x00110001,
};

/* Callback message codes (passed to ECICallback as msg). */
enum ECIMessage {
    eciWaveformBuffer    = 0,
    eciPhonemeBuffer     = 1,
    eciIndexReply        = 2,
    eciPhonemeIndexReply = 3,
    eciWordIndexReply    = 4,
    eciStringIndexReply  = 5,
    eciAudioIndexReply   = 6,
    eciSynthesisBreak    = 7,
};

/* Callback return codes */
enum ECICallbackReturn {
    eciDataNotProcessed = 0,
    eciDataProcessed    = 1,
    eciDataAbort        = 2,
};

/* Error bits (returned by eciProgStatus) */
enum ECIError {
    ECI_NOERROR           = 0x00000000,
    ECI_SYSTEMERROR       = 0x00000001,
    ECI_MEMORYERROR       = 0x00000002,
    ECI_MODULELOADERROR   = 0x00000004,
    ECI_DELTAERROR        = 0x00000008,
    ECI_SYNTHERROR        = 0x00000010,
    ECI_DEVICEERROR       = 0x00000020,
    ECI_DICTERROR         = 0x00000040,
    ECI_PARAMETERERROR    = 0x00000080,
    ECI_SYNTHESIZINGERROR = 0x00000100,
    ECI_DEVICEBUSY        = 0x00000200,
    ECI_SYNTHESISPAUSED   = 0x00000400,
    ECI_REENTRANTCALL     = 0x00000800,
    ECI_ROMANIZERERROR    = 0x00001000,
    ECI_SYNTHESIZING      = 0x00002000,
};

typedef int (*ECICallback)(ECIHand h, int msg, long lParam, void *pData);

/*
 * The function pointer table populated by eci_runtime_open(). All ECI API
 * functions go through this so the module dlopen()s eci.so at runtime
 * rather than linking against it.
 */
typedef struct {
    void           *(*New)(void);
    void           *(*NewEx)(int language);
    int             (*Delete)(ECIHand h);
    void            (*Version)(char *buf);
    int             (*ProgStatus)(ECIHand h);
    char           *(*ErrorMessage)(ECIHand h, char *buf);
    int             (*Speaking)(ECIHand h);
    int             (*Stop)(ECIHand h);
    int             (*Reset)(ECIHand h);
    int             (*Synchronize)(ECIHand h);
    int             (*Synthesize)(ECIHand h);
    int             (*AddText)(ECIHand h, const char *text);
    int             (*ClearInput)(ECIHand h);
    int             (*Pause)(ECIHand h, int on);
    int             (*InsertIndex)(ECIHand h, int index);
    int             (*GetParam)(ECIHand h, int param);
    int             (*SetParam)(ECIHand h, int param, int value);
    int             (*GetVoiceParam)(ECIHand h, int voice, int param);
    int             (*SetVoiceParam)(ECIHand h, int voice, int param, int value);
    int             (*GetDefaultParam)(int param);
    int             (*SetDefaultParam)(int param, int value);
    int             (*GetVoiceName)(ECIHand h, int voice, char *name);
    int             (*SetVoiceName)(ECIHand h, int voice, const char *name);
    int             (*CopyVoice)(ECIHand h, int src, int dst);
    void            (*RegisterCallback)(ECIHand h, ECICallback cb, void *data);
    int             (*SetOutputBuffer)(ECIHand h, int size, void *buf);
    int             (*SetOutputDevice)(ECIHand h, int dev);
    int             (*SetOutputFilename)(ECIHand h, const char *filename);
} EciApi;

/* dlopen the ECI library at <eci_so_path>, populate api, return 0 on success.
 * On failure, errmsg (if non-NULL) is filled with a heap-allocated message
 * the caller must free. */
int  eci_runtime_open(const char *eci_so_path, EciApi *api, char **errmsg);
void eci_runtime_close(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_ELOQUENCE_ECI_H */
