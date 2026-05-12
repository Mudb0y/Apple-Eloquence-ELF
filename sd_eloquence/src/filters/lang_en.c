/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Derived from NVDA-IBMTTS-Driver/addon/synthDrivers/ibmeci.py
 *   english_ibm_fixes (lines 74-103) and english_fixes (lines 41-62), union'd.
 * Copyright (C) 2009-2026 David CM and contributors, GPL-2.0
 */
#include "filters.h"

#ifdef HAVE_PCRE2
#include <pcre2.h>

filter_rule lang_en_rules[] = {
    /* --- IBM rules first (english_ibm_fixes, lines 74-103) --- */

    /* ViaVoice doesn't like spaces in Mc names. */
    { "\\b(Mc)\\s+([A-Z][a-z]|[A-Z][A-Z]+)", "$1$2", 0, NULL },

    /* Crash word: caesur/cæsur */
    { "c(ae|\\xe6)sur(e)?", "seizur", PCRE2_CASELESS, NULL },

    /* h'r e / h'v e patterns */
    { "\\b(|\\d+|\\W+)h'(r|v)[e]", "$1h $2e", PCRE2_CASELESS, NULL },

    /* consonant-cluster + hhes + word (variant 1) */
    { "\\b(\\w+[bdfhjlmnqrvyz])(h[he]s)([abcdefghjklmnopqrstvwy]\\w+)\\b",
      "$1 $2$3", PCRE2_CASELESS, NULL },

    /* consonant-cluster + hhes + "iron..." */
    { "\\b(\\w+[bdfhjlmnqrvyz])(h[he]s)(iron+[degins]?)",
      "$1 $2$3", PCRE2_CASELESS, NULL },

    /* word with apostrophe-consonant + h+hes + trailing word (variant 1) */
    { "\\b(\\w+'{1,}[bcdfghjklmnpqrstvwxyz])'*(h+[he]s)([abcdefghijklmnopqrstvwy]\\w+)\\b",
      "$1 $2$3", PCRE2_CASELESS, NULL },

    /* word with consonant + apostrophe-h+hes + trailing word (variant 2) */
    { "\\b(\\w+[bcdfghjklmnpqrstvwxyz])('{1,}h+[he]s)([abcdefghijklmnopqrstvwy]\\w+)\\b",
      "$1 $2$3", PCRE2_CASELESS, NULL },

    /* digit:digit-digit-ordinal-suffix, e.g. "1:23rd" */
    { "(\\d):(\\d\\d[snrt][tdh])", "$1 $2", PCRE2_CASELESS, NULL },

    /* consonant-run with two apostrophes, e.g. contractions */
    { "\\b([bcdfghjklmnpqrstvwxz]+)'([bcdefghjklmnpqrstvwxz']+)'([drtv][aeiou]?)",
      "$1 $2 $3", PCRE2_CASELESS, NULL },

    /* you're've-like clusters */
    { "\\b(you+)'(re)+'([drv]e?)", "$1 $2 $3", PCRE2_CASELESS, NULL },

    /* re/un/non/anti + cosp -> kosp */
    { "(re|un|non|anti)cosp", "$1kosp", PCRE2_CASELESS, NULL },

    /* tzsche crash pattern */
    { "\\b(\\d+|\\W+)?(\\w+\\_+)?(\\_+)?([bcdfghjklmnpqrstvwxz]+)?(\\d+)?t+z[s]che",
      "$1 $2 $3 $4 $5 tz sche", PCRE2_CASELESS, NULL },

    /* juar + 9+ letter suffix */
    { "(juar)([a-z']{9,})", "$1 $2", PCRE2_CASELESS, NULL },

    /* ViaVoice-specific: URL pattern crash */
    { "(http://|ftp://)([a-z]+)(\\W){1,3}([a-z]+)(/*\\W){1,3}([a-z]){1}",
      "$1$2$3$4 $5$6", PCRE2_CASELESS, NULL },

    /* floating-point arithmetic crash: digits op digits.digits.00+ */
    { "(\\d+)([-+*^/])(\\d+)(\\.)(\\d+)(\\.)((0){2,})",
      "$1$2$3$4$5$6 $7", PCRE2_CASELESS, NULL },

    /* floating-point arithmetic crash: digits op digits.digits.0<non-digit> */
    { "(\\d+)([-+*^/])(\\d+)(\\.)(\\d+)(\\.)(0\\W)",
      "$1$2$3$4 $5$6$7", PCRE2_CASELESS, NULL },

    /* arithmetic crash: digits op+ digits op+ [,.+] 00+ */
    { "(\\d+)([-+*^/]+)(\\d+)([-+*^/]+)([,.+])(0{2,})",
      "$1$2$3$4$5 $6", PCRE2_CASELESS, NULL },

    /* multi-dot float crash */
    { "(\\d+)(\\.+)(\\d+)(\\.+)(0{2,})(\\.\\d*)\\s*\\.*([-+*^/])",
      "$1$2$3$4 $5$6$7", PCRE2_CASELESS, NULL },

    /* comma-separated thousands crash (00 form) */
    { "(\\d+)\\s*([-+*^/])\\s*(\\d+)(,)(00\\b)",
      "$1$2$3$4 $5", PCRE2_CASELESS, NULL },

    /* comma-separated thousands crash (0000+ form) */
    { "(\\d+)\\s*([-+*^/])\\s*(\\d+)(,)(0{4,})",
      "$1$2$3$4 $5", PCRE2_CASELESS, NULL },

    /* comma-hundred fix: 3-digit,000,3-digit */
    { "\\b(\\d{1,3}),(000),(\\d{1,3})\\b",
      "$1$2$3", 0, NULL },

    /* comma-hundred fix: 3-digit,000,3-digit,3-digit */
    { "\\b(\\d{1,3}),(000),(\\d{1,3}),(\\d{1,3})\\b",
      "$1$2$3$4", 0, NULL },

    /* comma-hundred fix: 3-digit,000,3-digit,3-digit,3-digit */
    { "\\b(\\d{1,3}),(000),(\\d{1,3}),(\\d{1,3}),(\\d{1,3})\\b",
      "$1$2$3$4$5", 0, NULL },

    /* comma-hundred fix: 3-digit,000,3-digit,3-digit,3-digit,3-digit */
    { "\\b(\\d{1,3}),(000),(\\d{1,3}),(\\d{1,3}),(\\d{1,3}),(\\d{1,3})\\b",
      "$1$2$3$4$5$6", 0, NULL },

    /* --- Non-IBM rules (english_fixes, lines 41-62) not already in IBM list --- */

    /* Date parser bug: "03 Marble" -> "march threerd ble". Add extra space. */
    { "\\b(\\d+) (Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Sept|Oct|Nov|Dec)([a-z]+)",
      "$1  $2$3", 0, NULL },

    /* Don't break UK formatted dates (undo double-space if full month name follows). */
    { "\\b(\\d+)  (January|February|March|April|May|June|July|August|September|October|November|December)\\b",
      "$1 $2", 0, NULL },

    /* EUR + digits */
    { "(EUR[A-Z]+)(\\d+)", "$1 $2", PCRE2_CASELESS, NULL },

    { NULL, NULL, 0, NULL }
};

#else
filter_rule lang_en_rules[] = { { NULL, NULL, 0, NULL } };
#endif
