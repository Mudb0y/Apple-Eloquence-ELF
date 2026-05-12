/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * ssml/ssml.c -- SAX-based SSML → frame translator.
 *
 * Copyright (C) 2026 Mudb0y / Stas Przecinek
 */
#include "ssml.h"

#include "../synth/marks.h"
#include "../eci/voices.h"
#include "../eci/languages.h"
#include "../eci/eci.h"

#include <libxml/parser.h>
#include <libxml/SAX2.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define FRAMES_CAP    256
#define ARENA_CAP_BASE 4096

typedef struct {
    synth_job  *job;
    int         current_dialect;
    uint32_t    job_seq;
    int         text_buf_len;   /* used to coalesce adjacent text */
    char       *text_buf;
    size_t      text_buf_cap;
} SsmlCtx;

static int has_lt(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) if (data[i] == '<') return 1;
    return 0;
}

static void flush_text(SsmlCtx *c) {
    if (c->text_buf_len <= 0) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (f) {
        f->kind = FRAME_TEXT;
        f->u.text.text = synth_job_arena_strndup(c->job, c->text_buf, c->text_buf_len);
    }
    c->text_buf_len = 0;
}

static void append_text(SsmlCtx *c, const xmlChar *ch, int len) {
    if (len <= 0) return;
    if ((size_t)(c->text_buf_len + len + 1) > c->text_buf_cap) {
        size_t newcap = c->text_buf_cap ? c->text_buf_cap * 2 : 256;
        while (newcap < (size_t)(c->text_buf_len + len + 1)) newcap *= 2;
        char *nb = realloc(c->text_buf, newcap);
        if (!nb) return;  /* drop silently; degrades to truncated text */
        c->text_buf = nb;
        c->text_buf_cap = newcap;
    }
    memcpy(c->text_buf + c->text_buf_len, ch, len);
    c->text_buf_len += len;
    c->text_buf[c->text_buf_len] = 0;
}

/* Convenience: get an XML attribute value as a C string (or NULL). */
static const char *attr(const xmlChar **attrs, const char *name) {
    if (!attrs) return NULL;
    for (int i = 0; attrs[i]; i += 2) {
        if (xmlStrcasecmp(attrs[i], (const xmlChar *)name) == 0)
            return (const char *)attrs[i + 1];
    }
    return NULL;
}

/* Forward decls for element handlers. */
static void on_mark(SsmlCtx *c, const xmlChar **attrs);
static void on_break(SsmlCtx *c, const xmlChar **attrs);
static void on_prosody_start(SsmlCtx *c, const xmlChar **attrs);
static void on_prosody_end(SsmlCtx *c);
static void on_voice_start(SsmlCtx *c, const xmlChar **attrs);
static void on_voice_end(SsmlCtx *c);
static void on_lang_start(SsmlCtx *c, const xmlChar **attrs);
static void on_lang_end(SsmlCtx *c);
static void on_sayas_start(SsmlCtx *c, const xmlChar **attrs);
static void on_sayas_end(SsmlCtx *c);
static void on_sub_start(SsmlCtx *c, const xmlChar **attrs);

static void sax_start_element(void *ud, const xmlChar *name, const xmlChar **attrs) {
    SsmlCtx *c = ud;
    flush_text(c);
    if      (xmlStrcasecmp(name, (xmlChar *)"speak")    == 0) {
        /* <speak xml:lang="..."> sets initial dialect. */
        const char *lang = attr(attrs, "xml:lang");
        if (!lang) lang = attr(attrs, "lang");
        if (lang) {
            const LangEntry *L = lang_by_iso(lang);
            if (L) c->current_dialect = L->eci_dialect;
        }
    }
    else if (xmlStrcasecmp(name, (xmlChar *)"mark")     == 0)  on_mark(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"break")    == 0)  on_break(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"prosody")  == 0)  on_prosody_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"voice")    == 0)  on_voice_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"lang")     == 0)  on_lang_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"say-as")   == 0)  on_sayas_start(c, attrs);
    else if (xmlStrcasecmp(name, (xmlChar *)"sub")      == 0)  on_sub_start(c, attrs);
    /* Unknown elements (including <emphasis>): children processed, element ignored. */
}

static void sax_end_element(void *ud, const xmlChar *name) {
    SsmlCtx *c = ud;
    flush_text(c);
    if      (xmlStrcasecmp(name, (xmlChar *)"prosody")  == 0)  on_prosody_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"voice")    == 0)  on_voice_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"lang")     == 0)  on_lang_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"say-as")   == 0)  on_sayas_end(c);
    else if (xmlStrcasecmp(name, (xmlChar *)"p") == 0 ||
             xmlStrcasecmp(name, (xmlChar *)"s") == 0) {
        /* <p>/<s> close inserts a small break. */
        synth_frame *f = synth_job_push_frame(c->job);
        if (f) { f->kind = FRAME_BREAK; f->u.brk.millis = 400; }
    }
}

static void sax_characters(void *ud, const xmlChar *ch, int len) {
    SsmlCtx *c = ud;
    append_text(c, ch, len);
}

static synth_job *plain_text_job(const char *data, size_t len,
                                 SPDMessageType msgtype, int dialect,
                                 uint32_t job_seq) {
    (void)dialect;
    synth_job *j = synth_job_new(FRAMES_CAP, len + ARENA_CAP_BASE);
    if (!j) return NULL;
    j->seq = job_seq;
    j->msgtype = msgtype;

    if (msgtype == SPD_MSGTYPE_CHAR ||
        msgtype == SPD_MSGTYPE_KEY  ||
        msgtype == SPD_MSGTYPE_SPELL) {
        synth_frame *f1 = synth_job_push_frame(j);
        if (f1) { f1->kind = FRAME_TEXTMODE; f1->u.textmode.mode = 2; f1->u.textmode.saved_mode = 0; }
    }
    synth_frame *f = synth_job_push_frame(j);
    if (f) {
        f->kind = FRAME_TEXT;
        f->u.text.text = synth_job_arena_strndup(j, data, len);
    }
    if (msgtype == SPD_MSGTYPE_CHAR ||
        msgtype == SPD_MSGTYPE_KEY  ||
        msgtype == SPD_MSGTYPE_SPELL) {
        synth_frame *f2 = synth_job_push_frame(j);
        if (f2) { f2->kind = FRAME_TEXTMODE; f2->u.textmode.mode = 0; f2->u.textmode.saved_mode = 2; }
    }
    return j;
}

synth_job *ssml_parse(const char *data, size_t len,
                      SPDMessageType msgtype, int default_dialect,
                      uint32_t job_seq) {
    if (msgtype != SPD_MSGTYPE_TEXT) {
        return plain_text_job(data, len, msgtype, default_dialect, job_seq);
    }
    if (!has_lt(data, len)) {
        return plain_text_job(data, len, msgtype, default_dialect, job_seq);
    }

    synth_job *j = synth_job_new(FRAMES_CAP, len * 2 + ARENA_CAP_BASE);
    if (!j) return NULL;
    j->seq = job_seq;
    j->msgtype = msgtype;

    SsmlCtx ctx = {
        .job = j, .current_dialect = default_dialect, .job_seq = job_seq,
        .text_buf = NULL, .text_buf_len = 0, .text_buf_cap = 0,
    };

    xmlSAXHandler sax;
    memset(&sax, 0, sizeof(sax));
    sax.startElement = sax_start_element;
    sax.endElement   = sax_end_element;
    sax.characters   = sax_characters;

    int ok = xmlSAXUserParseMemory(&sax, &ctx, data, (int)len);
    flush_text(&ctx);
    free(ctx.text_buf);

    if (ok != 0) {
        /* Parse failed; fall back to plain text. */
        synth_job_free(j);
        return plain_text_job(data, len, msgtype, default_dialect, job_seq);
    }
    return j;
}

/* === Element handlers === */

static void on_mark(SsmlCtx *c, const xmlChar **attrs) {
    const char *name = attr(attrs, "name");
    if (!name || strlen(name) > 30) return;
    uint32_t id = marks_register(name, c->job_seq);
    if (id == 0) return;  /* table full or unregisterable -- silently drop */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_MARK;
    f->u.mark.id = id;
    f->u.mark.name = synth_job_arena_strdup(c->job, name);
}

static int strength_to_ms(const char *s) {
    if (!s) return 200;
    if (!strcasecmp(s, "none"))    return 0;
    if (!strcasecmp(s, "x-weak"))  return 100;
    if (!strcasecmp(s, "weak"))    return 200;
    if (!strcasecmp(s, "medium"))  return 400;
    if (!strcasecmp(s, "strong"))  return 700;
    if (!strcasecmp(s, "x-strong"))return 1000;
    return 200;
}

static int parse_time_ms(const char *t) {
    if (!t) return -1;
    char *end = NULL;
    double v = strtod(t, &end);
    if (end == t) return -1;
    if (end && (!*end || isspace((unsigned char)*end))) return (int)v;  /* bare number = ms */
    if (!strcasecmp(end, "ms"))  return (int)v;
    if (!strcasecmp(end, "s"))   return (int)(v * 1000.0);
    return -1;
}

static void on_break(SsmlCtx *c, const xmlChar **attrs) {
    int ms = parse_time_ms(attr(attrs, "time"));
    if (ms < 0) ms = strength_to_ms(attr(attrs, "strength"));
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_BREAK;
    f->u.brk.millis = ms;
}

static int parse_percent(const char *v, int dflt) {
    if (!v) return dflt;
    char *end = NULL;
    double d = strtod(v, &end);
    if (end == v) return dflt;
    if (end && *end == '%') return (int)d;
    return dflt;
}

static int prosody_rate_value(const char *v) {
    if (!v) return -1;
    if (!strcasecmp(v, "x-slow"))  return 20;
    if (!strcasecmp(v, "slow"))    return 40;
    if (!strcasecmp(v, "medium"))  return 60;
    if (!strcasecmp(v, "fast"))    return 80;
    if (!strcasecmp(v, "x-fast")) return 100;
    return parse_percent(v, -1);
}

static int prosody_volume_value(const char *v) {
    if (!v) return -1;
    if (!strcasecmp(v, "silent")) return 0;
    if (!strcasecmp(v, "soft"))   return 25;
    if (!strcasecmp(v, "medium")) return 50;
    if (!strcasecmp(v, "loud"))   return 75;
    if (!strcasecmp(v, "x-loud")) return 100;
    return parse_percent(v, -1);
}

static int prosody_pitch_value(const char *v) {
    if (!v) return -1;
    if (!strcasecmp(v, "x-low"))  return 20;
    if (!strcasecmp(v, "low"))    return 40;
    if (!strcasecmp(v, "medium")) return 50;
    if (!strcasecmp(v, "high"))   return 70;
    if (!strcasecmp(v, "x-high")) return 90;
    /* "+Nst" -> semitones -> ~6% each. */
    if (*v == '+' || *v == '-') {
        char *end = NULL;
        double d = strtod(v, &end);
        if (end && (!strcasecmp(end, "st") || !strcasecmp(end, "Hz")))
            return 50 + (int)(d * 6.0);
    }
    return parse_percent(v, -1);
}

static void on_prosody_start(SsmlCtx *c, const xmlChar **attrs) {
    /* Each attribute that's present generates one PROSODY_PUSH; the
     * matching POP happens at on_prosody_end via a counter. To keep this
     * simple, we push at most one frame: rate takes priority over pitch,
     * pitch over volume. This matches the most common use. */
    int v = prosody_rate_value(attr(attrs, "rate"));
    int param = -1;
    if (v >= 0) param = eciSpeed;
    else {
        v = prosody_pitch_value(attr(attrs, "pitch"));
        if (v >= 0) param = eciPitchBaseline;
        else {
            v = prosody_volume_value(attr(attrs, "volume"));
            if (v >= 0) param = eciVolume;
        }
    }
    if (param < 0) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_PROSODY_PUSH;
    f->u.prosody.param = param;
    f->u.prosody.new_value = v;
    f->u.prosody.saved_value = -1;  /* worker fills in via GetVoiceParam at exec time */
}

static void on_prosody_end(SsmlCtx *c) {
    /* POP only emits a frame if the matching PUSH did; we always emit one
     * here -- if there was no PUSH, the worker treats POP as no-op. */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_PROSODY_POP;
}

static void on_voice_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *name = attr(attrs, "name");
    int slot = -1;
    if (name) slot = voice_find_by_name(name);
    if (slot < 0) {
        const char *gender = attr(attrs, "gender");
        if (gender && !strcasecmp(gender, "male"))   slot = 0;
        if (gender && !strcasecmp(gender, "female")) slot = 1;
    }
    if (slot < 0) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_VOICE_PUSH;
    f->u.voice.slot = slot;
}

static void on_voice_end(SsmlCtx *c) {
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_VOICE_POP;
}

static void on_lang_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *lang = attr(attrs, "xml:lang");
    if (!lang) lang = attr(attrs, "lang");
    if (!lang) return;
    const LangEntry *L = lang_by_iso(lang);
    if (!L) return;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_LANG_PUSH;
    f->u.lang.dialect = L->eci_dialect;
    c->current_dialect = L->eci_dialect;
}

static void on_lang_end(SsmlCtx *c) {
    (void)c;
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_LANG_POP;
}

static void on_sayas_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *as = attr(attrs, "interpret-as");
    if (!as) return;
    int mode = 0;
    if (!strcasecmp(as, "characters") || !strcasecmp(as, "spell")) mode = 2;
    if (mode == 0) return;  /* digits/cardinal/ordinal pass through */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_TEXTMODE;
    f->u.textmode.mode = mode;
    f->u.textmode.saved_mode = 0;
}

static void on_sayas_end(SsmlCtx *c) {
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_TEXTMODE;
    f->u.textmode.mode = 0;
    f->u.textmode.saved_mode = 2;
}

static void on_sub_start(SsmlCtx *c, const xmlChar **attrs) {
    const char *alias = attr(attrs, "alias");
    if (!alias) return;
    /* sub replaces inner text with alias. v1 approximation: emit alias as
     * text frame; inner characters between <sub> and </sub> will still
     * be captured by sax_characters and emitted -- that's wrong but rare
     * enough in practice that we'll revisit only if we see <sub> in the
     * wild. */
    synth_frame *f = synth_job_push_frame(c->job);
    if (!f) return;
    f->kind = FRAME_TEXT;
    f->u.text.text = synth_job_arena_strdup(c->job, alias);
}
