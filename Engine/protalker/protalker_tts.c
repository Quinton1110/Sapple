/* IBM ProTALKER 97 Japanese TTS engine (読み秀君 / 読み子ちゃん) running in the SAPI4/emu x86 interpreter.
 *
 * Drives IBM's 32-bit Tts_mgr.dll (spcSynInit / spcSynCntl / spcSynSpeak / spcSynTerm) directly in SAPI4/emu, with
 * regtts.dll path queries answered by host thunks (SAPI4/emu/winapi.c) mapped to ProTalkerVoice/{DICT,UNIT}. Clause
 * pauses (PauseKuTen=2, PauseToTen=5, PauseKaiGyo=5) and translate flags (0x004D) match Ptengine.dll. */
#include "protalker_tts.h"
#include "emu.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <strings.h>
#elif defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <strings.h>
#endif

#define PROTALKER_MEM_SIZE 0x08000000u /* 128 MB guest virtual address space (EMU_HEAP_BASE = 64 MB) */

struct protalker_tts {
    Emu *e;
    EmuModule *mod;
    int actor; /* 1 = 読み秀君 (WFCM1000), 2 = 読み子ちゃん (WFCF1000) */
    uint32_t fn_init, fn_cntl, fn_speak, fn_term;
    uint32_t g_err, g_handle, g_user_str, g_cntl;
    uint32_t handle_val;
    uint32_t cb_thunk;
    int cur_speed, cur_pitch;

    protalker_audio_cb cur_cb;
    void *cur_user;
    int stopped;
    int16_t zero_buf[2048];
};

static int actor_for_voice(const char *voice_name)
{
    if (!voice_name) return 0;
    if (!strcmp(voice_name, "ProTalker_Hide") || !strcmp(voice_name, "ProTalker_Yomihide") ||
        !strcmp(voice_name, "読み秀君"))
        return 1;
    if (!strcmp(voice_name, "ProTalker_Ko") || !strcmp(voice_name, "ProTalker_Yomiko") ||
        !strcmp(voice_name, "読み子ちゃん"))
        return 2;
    return 0;
}

static int find_entry_ci(const char *dir, const char *want, char *out, size_t out_sz)
{
    struct stat st;
    if (!dir || !*dir || !want || !*want) return 0;
    snprintf(out, out_sz, "%s/%s", dir, want);
    if (stat(out, &st) == 0) return 1;
#ifndef _WIN32
    {
        DIR *d = opendir(dir);
        struct dirent *e;
        if (!d) return 0;
        while ((e = readdir(d)) != NULL) {
            if (strcasecmp(e->d_name, want) == 0) {
                snprintf(out, out_sz, "%s/%s", dir, e->d_name);
                closedir(d);
                return stat(out, &st) == 0;
            }
        }
        closedir(d);
    }
#endif
    return 0;
}

static int check_protalker_dir(const char *dir, char *out_dll, size_t dll_sz)
{
    char sub[1100], file[1200];
    struct stat st;
    if (!find_entry_ci(dir, "Tts_mgr.dll", out_dll, dll_sz)) return 0;
    if (stat(out_dll, &st) != 0 || st.st_size <= 0) return 0;
    if (!find_entry_ci(dir, "DICT", sub, sizeof sub)) return 0;
    if (!find_entry_ci(sub, "ACC2.DCT", file, sizeof file)) return 0;
    if (!find_entry_ci(dir, "UNIT", sub, sizeof sub)) return 0;
    if (!find_entry_ci(sub, "WFCM1000.DAT", file, sizeof file)) return 0;
    if (!find_entry_ci(sub, "WFCF1000.DAT", file, sizeof file)) return 0;
    return 1;
}

int protalker_tts_has_data(const char *dir)
{
    char dll[1200];
    return check_protalker_dir(dir, dll, sizeof dll);
}

static void on_guest_pcm(Emu *e, X86 *c)
{
    protalker_tts *t = (protalker_tts *)emu_user(e);
    uint32_t pcm_ptr = x86_arg(c, 0);
    uint32_t byte_len = x86_arg(c, 1);
    size_t samples;
    if (!t || t->stopped || !t->cur_cb) {
        c->s.r[EAX] = 0;
        return;
    }
    samples = byte_len / 2;
    if (samples == 0) {
        c->s.r[EAX] = byte_len;
        return;
    }
    if (pcm_ptr != 0) {
        const int16_t *pcm = (const int16_t *)emu_ptr(e, pcm_ptr, byte_len);
        if (t->cur_cb(pcm, samples, t->cur_user)) {
            t->stopped = 1;
            c->s.r[EAX] = 0;
            return;
        }
    } else {
        size_t rem = samples;
        while (rem > 0) {
            size_t chunk = rem > 2048 ? 2048 : rem;
            if (t->cur_cb(t->zero_buf, chunk, t->cur_user)) {
                t->stopped = 1;
                c->s.r[EAX] = 0;
                return;
            }
            rem -= chunk;
        }
    }
    c->s.r[EAX] = byte_len;
}

/* spcSynCntl parameter layout (verified from Ptengine.dll 0x1000244d & Tts_mgr.dll 0x1001a647):
 *   +0x00: speed     (4..16, SAPI 4 default 9; Sapple default 6 = NVDA rate 25 / dwSpeed 120)
 *   +0x04: pitch     (50..170, SAPI 4 default 80; Sapple default 97 = NVDA pitch 55.5)
 *   +0x08: volume    (0..9, default 9 = 100% synthesis level)
 *   +0x0C: accent    (0..9, default 5 = standard intonation strength)
 *   +0x10: actor     (1 = 読み秀君 WFCM1000, 2 = 読み子ちゃん WFCF1000)
 *   +0x14: fixed     (1)
 *   +0x18: translate (0x004D = 0x0C | 0x41 in Ptengine.dll: English word & digit-place conversion) */
static void apply_cntl(protalker_tts *t, int speed, int pitch, int volume, int accent, uint16_t mask)
{
    X86 *c = emu_cpu(t->e);
    uint32_t args[2];
    wr32(c, t->g_cntl + 0x00, (uint32_t)speed);
    wr32(c, t->g_cntl + 0x04, (uint32_t)pitch);
    wr32(c, t->g_cntl + 0x08, (uint32_t)volume);
    wr32(c, t->g_cntl + 0x0C, (uint32_t)accent);
    wr32(c, t->g_cntl + 0x10, (uint32_t)t->actor);
    wr32(c, t->g_cntl + 0x14, 1u);
    wr16(c, t->g_cntl + 0x18, mask);
    args[0] = t->handle_val;
    args[1] = t->g_cntl;
    emu_call(t->e, t->fn_cntl, 2, args);
    t->cur_speed = speed;
    t->cur_pitch = pitch;
}

protalker_tts *protalker_tts_open(const char *dir, const char *voice_name, char *err, size_t errlen)
{
    char dll_path[1200];
    int actor = actor_for_voice(voice_name);
    protalker_tts *t;
    uint32_t init_args[4], rc;
    if (!actor) {
        if (err && errlen) snprintf(err, errlen, "unknown ProTalker voice");
        return NULL;
    }
    if (!check_protalker_dir(dir, dll_path, sizeof dll_path)) {
        if (err && errlen) snprintf(err, errlen, "incomplete or missing ProTalkerVoice data");
        return NULL;
    }
    t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->actor = actor;
    t->e = emu_create(PROTALKER_MEM_SIZE);
    if (!t->e) {
        if (err && errlen) snprintf(err, errlen, "emu_create failed");
        free(t);
        return NULL;
    }
    emu_set_user(t->e, t);
    emu_map_dir(t->e, "C:\\PROTALK", dir);
    t->mod = emu_load_pe(t->e, dll_path, "C:\\PROTALK\\Tts_mgr.dll", EMU_IMAGE_BASE);
    if (!t->mod || !emu_dll_attach(t->e, t->mod)) {
        if (err && errlen) snprintf(err, errlen, "failed to load Tts_mgr.dll: %s", emu_error(t->e));
        emu_destroy(t->e);
        free(t);
        return NULL;
    }
    t->fn_init = emu_get_export(t->e, t->mod, "spcSynInit");
    t->fn_cntl = emu_get_export(t->e, t->mod, "spcSynCntl");
    t->fn_speak = emu_get_export(t->e, t->mod, "spcSynSpeak");
    t->fn_term = emu_get_export(t->e, t->mod, "spcSynTerm");
    if (!t->fn_init || !t->fn_cntl || !t->fn_speak || !t->fn_term) {
        if (err && errlen) snprintf(err, errlen, "missing spcSyn* exports in Tts_mgr.dll");
        emu_destroy(t->e);
        free(t);
        return NULL;
    }
    t->g_err = emu_static(t->e, 16);
    t->g_handle = emu_static(t->e, 16);
    t->g_user_str = emu_strdup(t->e, "USER");
    t->g_cntl = emu_static(t->e, 64);
    t->cb_thunk = emu_thunk(t->e, "protalker_pcm_cb", on_guest_pcm, 8);

    init_args[0] = 0x0000F001u;
    init_args[1] = t->g_err;
    init_args[2] = t->g_handle;
    init_args[3] = t->g_user_str;
    rc = emu_call(t->e, t->fn_init, 4, init_args);
    t->handle_val = rd32(emu_cpu(t->e), t->g_handle);
    if (emu_failed(t->e) || rc != 0 || !t->handle_val) {
        if (err && errlen) snprintf(err, errlen, "spcSynInit failed (rc=%u, err=%u)", rc, rd32(emu_cpu(t->e), t->g_err));
        emu_destroy(t->e);
        free(t);
        return NULL;
    }
    apply_cntl(t, 6, 97, 9, 5, 0x004Du);
    return t;
}

void protalker_tts_close(protalker_tts *t)
{
    if (!t) return;
    if (t->e && t->fn_term && t->handle_val && !emu_failed(t->e)) {
        uint32_t args[1] = {t->handle_val};
        emu_call(t->e, t->fn_term, 1, args);
    }
    if (t->e) emu_destroy(t->e);
    free(t);
}

static char *utf8_to_sjis_alloc(const char *utf8)
{
    if (!utf8 || !*utf8) {
        char *e = calloc(1, 1);
        return e;
    }
#ifdef __APPLE__
    {
        CFStringRef cf = CFStringCreateWithCString(kCFAllocatorDefault, utf8, kCFStringEncodingUTF8);
        CFIndex len, max_sz, used = 0;
        char *out;
        if (!cf) return NULL;
        len = CFStringGetLength(cf);
        max_sz = CFStringGetMaximumSizeForEncoding(len, kCFStringEncodingDOSJapanese) + 4;
        if (max_sz < 16) max_sz = 16;
        out = calloc(1, (size_t)max_sz);
        if (out) {
            CFStringGetBytes(cf, CFRangeMake(0, len), kCFStringEncodingDOSJapanese, ' ', false, (UInt8 *)out,
                             max_sz - 1, &used);
            out[used] = '\0';
        }
        CFRelease(cf);
        return out;
    }
#elif defined(_WIN32)
    {
        int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
        wchar_t *wbuf;
        int slen;
        char *out;
        if (wlen <= 0) return NULL;
        wbuf = malloc((size_t)wlen * sizeof(wchar_t));
        if (!wbuf) return NULL;
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, wbuf, wlen);
        slen = WideCharToMultiByte(932, 0, wbuf, -1, NULL, 0, " ", NULL);
        if (slen <= 0) {
            free(wbuf);
            return NULL;
        }
        out = calloc(1, (size_t)slen + 4);
        if (out) WideCharToMultiByte(932, 0, wbuf, -1, out, slen, " ", NULL);
        free(wbuf);
        return out;
    }
#else
    {
        size_t n = strlen(utf8);
        char *out = malloc(n + 1);
        if (out) memcpy(out, utf8, n + 1);
        return out;
    }
#endif
}

static int is_sjis_lead(uint8_t c)
{
    return (c >= 0x81 && c <= 0x9F) || (c >= 0xE0 && c <= 0xFC);
}

static int emit_silence_samples(protalker_tts *t, size_t samples)
{
    while (samples > 0 && !t->stopped) {
        size_t chunk = samples > 2048 ? 2048 : samples;
        if (t->cur_cb(t->zero_buf, chunk, t->cur_user)) {
            t->stopped = 1;
            return 1;
        }
        samples -= chunk;
    }
    return t->stopped ? 1 : 0;
}

int protalker_tts_speak(protalker_tts *t, const char *utf8_text, double sapi_rate, double semitones,
                        protalker_audio_cb cb, void *user)
{
    char *sjis;
    int speed, pitch, rc_err = 0;
    const uint8_t *p;
    if (!t || !utf8_text || !cb || emu_failed(t->e)) return -1;
    sjis = utf8_to_sjis_alloc(utf8_text);
    if (!sjis) return -1;
    if (!*sjis) {
        free(sjis);
        return 0;
    }

    if (!(sapi_rate == sapi_rate)) sapi_rate = 0.0;
    if (!(semitones == semitones)) semitones = 0.0;

    /* Map SAPI rate (-10..18, default 0) to ProTALKER speed (4..16, default 6 = NVDA rate 25 / dwSpeed 120) */
    if (sapi_rate < 0.0) {
        speed = (int)lrint(6.0 + (sapi_rate / 10.0) * (6.0 - 4.0));
    } else {
        speed = (int)lrint(6.0 + (sapi_rate / 18.0) * (16.0 - 6.0));
    }
    if (speed < 4) speed = 4;
    if (speed > 16) speed = 16;

    /* Map semitones (-12..12, default 0) to ProTALKER pitch (50..170, default 97 = NVDA pitch 55.5) */
    if (semitones < 0.0) {
        pitch = (int)lrint(97.0 + (semitones / 12.0) * (97.0 - 50.0));
    } else {
        pitch = (int)lrint(97.0 + (semitones / 12.0) * (170.0 - 97.0));
    }
    if (pitch < 50) pitch = 50;
    if (pitch > 170) pitch = 170;

    t->cur_cb = cb;
    t->cur_user = user;
    t->stopped = 0;

    /* Split clauses and insert PauseKuTen=2 / PauseToTen=5 / PauseKaiGyo=5 as Ptengine.dll (0x10004faa) does */
    p = (const uint8_t *)sjis;
    while (*p && !t->stopped) {
        const uint8_t *start;
        int pause_units = 0;
        size_t clen;
        while (*p == '\r' || *p == '\n' || *p == ' ') p++;
        if (!*p) break;
        start = p;
        while (*p) {
            uint8_t c0 = p[0];
            if (is_sjis_lead(c0) && p[1]) {
                uint8_t c1 = p[1];
                p += 2;
                if (c0 == 0x81 && (c1 == 0x42 || c1 == 0x44)) { /* 。 or ． */
                    pause_units = 5;
                    break;
                }
                if (c0 == 0x81 && (c1 == 0x41 || c1 == 0x43)) { /* 、 or ， */
                    pause_units = 2;
                    break;
                }
            } else {
                p++;
                if (c0 == 0xA1) { /* halfwidth ｡ */
                    pause_units = 5;
                    break;
                }
                if (c0 == 0xA4) { /* halfwidth ､ */
                    pause_units = 2;
                    break;
                }
                if (c0 == '\r' || c0 == '\n') {
                    if (c0 == '\r' && *p == '\n') p++;
                    pause_units = 5;
                    break;
                }
            }
        }
        clen = (size_t)(p - start);
        if (clen > 0) {
            char *clause = malloc(clen + 1);
            uint32_t g_text, speak_args[6], rc;
            if (!clause) {
                rc_err = -1;
                break;
            }
            memcpy(clause, start, clen);
            clause[clen] = '\0';

            apply_cntl(t, speed, pitch, 9, 5, 0x004Du);
            g_text = emu_strdup(t->e, clause);
            free(clause);
            if (!g_text) {
                rc_err = -1;
                break;
            }
            speak_args[0] = t->handle_val;
            speak_args[1] = g_text;
            speak_args[2] = 0;
            speak_args[3] = 0;
            speak_args[4] = 999u;
            speak_args[5] = t->cb_thunk;
            rc = emu_call(t->e, t->fn_speak, 6, speak_args);
            emu_free(t->e, g_text);
            if (emu_failed(t->e) || (rc != 0 && !t->stopped)) {
                rc_err = -1;
                break;
            }
        }
        if (pause_units > 0 && !t->stopped) {
            size_t sil_samples = (size_t)((22050 * pause_units) / speed);
            emit_silence_samples(t, sil_samples);
        }
    }

    free(sjis);
    t->cur_cb = NULL;
    t->cur_user = NULL;

    if (t->stopped) {
        /* Reset the spcSyn session so no partial synthesis / overlap-add state leaks into the next utterance. */
        if (!emu_failed(t->e) && t->fn_term && t->fn_init && t->handle_val) {
            uint32_t term_args[1] = {t->handle_val};
            uint32_t init_args[4] = {0x0000F001u, t->g_err, t->g_handle, t->g_user_str};
            emu_call(t->e, t->fn_term, 1, term_args);
            if (emu_call(t->e, t->fn_init, 4, init_args) == 0 && !emu_failed(t->e)) {
                t->handle_val = rd32(emu_cpu(t->e), t->g_handle);
            }
        }
        return 1;
    }
    if (rc_err != 0) return -1;
    return 0;
}
