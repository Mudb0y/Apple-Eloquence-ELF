/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * eci/runtime.h -- dlopen-loaded view of the IBM ECI runtime.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#ifndef SD_ELOQUENCE_ECI_RUNTIME_H
#define SD_ELOQUENCE_ECI_RUNTIME_H

#include "eci.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Dictionary types (IBM SDK 6.7.4 §"Dictionaries"). Declared here because we
 * load the dict entry points via the same dlsym pass. */
typedef void *ECIDictHand;
#define NULL_DICT_HAND 0
enum ECIDictError {
    DictNoError = 0,
    DictFileNotFound,
    DictOutOfMemory,
    DictInternalError,
    DictNoEntry,
    DictErrLookUpKey,
    DictAccessError,
    DictInvalidVolume
};
enum ECIDictVolume {
    eciMainDict    = 0,
    eciRootDict    = 1,
    eciAbbvDict    = 2,
    eciMainDictExt = 3
};

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

    /* Single-shot speak */
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

    /* Languages */
    int       (*GetAvailableLanguages)(enum ECILanguageDialect *aLanguages,
                                       int *nLanguages);

    /* Dictionaries */
    ECIDictHand       (*NewDict)(ECIHand);
    ECIDictHand       (*GetDict)(ECIHand);
    enum ECIDictError (*SetDict)(ECIHand, ECIDictHand);
    ECIDictHand       (*DeleteDict)(ECIHand, ECIDictHand);
    enum ECIDictError (*LoadDict)(ECIHand, ECIDictHand,
                                  enum ECIDictVolume, ECIInputText pFilename);
} EciApi;

int  eci_runtime_open(const char *eci_so_path, EciApi *api, char **errmsg);
void eci_runtime_close(void);

#ifdef __cplusplus
}
#endif

#endif
