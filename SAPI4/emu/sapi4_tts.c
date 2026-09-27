/* sapi4_tts: see sapi4_tts.h. Drives the original SAPI 4 engine through its COM interfaces:
 * DllGetClassObject -> ITTSEnumA (Next / Select) -> ITTSCentralA (TextData, AudioReset, Register) and
 * ITTSAttributesA (pitch / speed), with host implementations of the audio device (IAudio + IAudioDest)
 * and the notification sinks. PCM leaves through IAudioDest::DataSet straight to the caller's callback. */
#include "sapi4_tts.h"
#include "emu_internal.h"

#include <setjmp.h>
#ifdef X86_LOCKSTEP
void emu_lockstep_enable(Emu *e);   // lockstep.c: Mac-only differential test against Unicorn
#endif

typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } GUIDh;
static const GUIDh IID_IUnknown_ = { 0x00000000, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUIDh IID_IClassFactory_ = { 0x00000001, 0, 0, { 0xC0, 0, 0, 0, 0, 0, 0, 0x46 } };
static const GUIDh IID_IAudio_ = { 0xf546b340, 0xc743, 0x11cd, { 0x80, 0xe5, 0x0, 0xaa, 0x0, 0x3e, 0x4b, 0x50 } };
static const GUIDh IID_IAudioDest_ = { 0x2ec34da0, 0xc743, 0x11cd, { 0x80, 0xe5, 0x0, 0xaa, 0x0, 0x3e, 0x4b, 0x50 } };
static const GUIDh IID_ITTSEnumA_ = { 0x05EB6C6D, 0xDBAB, 0x11CD, { 0xB3, 0xCA, 0x00, 0xAA, 0x00, 0x47, 0xBA, 0x4F } };
static const GUIDh IID_ITTSAttributesA_ = { 0x0FD6E2A1, 0xE77D, 0x11CD, { 0xB3, 0xCA, 0x00, 0xAA, 0x00, 0x47, 0xBA, 0x4F } };
static const GUIDh IID_ITTSNotifySinkA_ = { 0x05EB6C6F, 0xDBAB, 0x11CD, { 0xB3, 0xCA, 0x00, 0xAA, 0x00, 0x47, 0xBA, 0x4F } };
static const GUIDh IID_ITTSNotifySinkW_ = { 0xC0FA8F40, 0x4A46, 0x101B, { 0x93, 0x1A, 0x00, 0xAA, 0x00, 0x47, 0xBA, 0x4F } };
static const GUIDh IID_ITTSBufNotifySink_ = { 0xe4963d40, 0xc743, 0x11cd, { 0x80, 0xe5, 0x0, 0xaa, 0x0, 0x3e, 0x4b, 0x50 } };
static const GUIDh CLSID_MSTTS_ = { 0xE0725551, 0x286F, 0x11d0, { 0x8E, 0x73, 0x00, 0xA0, 0xC9, 0x08, 0x33, 0x63 } };
/* L&H TruVoice American English (tv_enua.dll 6.0.0.10, 1998) */
static const GUIDh CLSID_TRUVOICE_ = { 0xCA141FD0, 0xAC7F, 0x11d1, { 0x97, 0xA3, 0x00, 0x60, 0x08, 0x27, 0x30, 0xFF } };

/* COM vtable slots */
enum { CF_CreateInstance = 3 };
enum { EN_Next = 3, EN_Select = 7 };
enum { TC_QueryInterface = 0, TC_TextData = 7, TC_AudioReset = 11, TC_Register = 12 };
enum { TA_PitchGet = 3, TA_PitchSet = 4, TA_SpeedGet = 7, TA_SpeedSet = 8 };

#define MEM_SIZE 0x10000000u
#define S4_MAX_MODES 32

struct s4_engine {
    Emu *e;
    EmuModule *mod;
    int kind;                   /* S4_ENGINE_MSTTS / S4_ENGINE_TRUVOICE */
    uint32_t central, attrs, enumr;
    uint32_t audio_obj, tts_sink, buf_sink, scratch, out_ptr;
    s4_mode mode;
    s4_limits lim;
    unsigned pitch_cur, speed_cur;
    char error[256];
    int dead;

    /* host audio device */
    uint32_t refs, sink;
    int started, claimed, start_notified, stop_notified;
    uint32_t bookmarks[1024];
    int nbook;
    uint64_t total_bytes, bytes_since_start;
    uint32_t rate, channels, bits;
    int format_set;

    /* notifications */
    int audio_stop_seen, textdata_done_seen;

    /* the utterance being spoken */
    s4_pcm_fn fn;
    void *user;
    int stopped, speaking;

    s4_stats last;
    int verbose;

    /* s4_list_modes */
    s4_mode *list;
    int nlist, maxlist;
};

static s4_engine *S(Emu *e) { return (s4_engine *)emu_user(e); }
#define A(i) x86_arg(c, (i))
#define LOGV(...) do { if (S(e)->verbose) EMU_LOG(__VA_ARGS__); } while (0)

static int guid_eq(Emu *e, uint32_t g, const GUIDh *h) { return memcmp(emu_ptr(e, g, 16), h, 16) == 0; }
static uint32_t guid_put(Emu *e, const GUIDh *h) { uint32_t a = emu_static(e, 16); memcpy(emu_ptr(e, a, 16), h, 16); return a; }

static void set_err(char *err, size_t errlen, const char *fmt, const char *arg) {
    if (!err || !errlen) return;
    snprintf(err, errlen, fmt, arg ? arg : "");
}

/* ------------------------------------------------------------------ host audio device (IAudio + IAudioDest) */
static void au_QI(Emu *e, X86 *c) {
    s4_engine *s = S(e);
    uint32_t riid = A(1), ppv = A(2), r = 0;
    if (guid_eq(e, riid, &IID_IUnknown_) || guid_eq(e, riid, &IID_IAudio_)) r = s->audio_obj;
    else if (guid_eq(e, riid, &IID_IAudioDest_)) r = s->audio_obj + 4;
    wr32(c, ppv, r);
    if (r) s->refs++;
    E_RET(r ? 0 : 0x80004002u);
}
static void au_AddRef(Emu *e, X86 *c) { E_RET(++S(e)->refs); }
static void au_Release(Emu *e, X86 *c) { E_RET(--S(e)->refs); }
static void au_ok(Emu *e, X86 *c) { (void)e; E_RET(0); }
static void au_Flush(Emu *e, X86 *c) { LOGV("[audio] Flush\n"); E_RET(0); }
static void au_LevelGet(Emu *e, X86 *c) { (void)e; if (A(1)) wr32(c, A(1), 0xFFFFFFFFu); E_RET(0); }
static void au_PassNotify(Emu *e, X86 *c) { S(e)->sink = A(1); E_RET(0); }
static void au_PosnGet(Emu *e, X86 *c) { if (A(1)) wr64(c, A(1), S(e)->total_bytes); E_RET(0); }
static void au_TotalGet(Emu *e, X86 *c) { if (A(1)) wr64(c, A(1), S(e)->total_bytes); E_RET(0); }
static void au_Claim(Emu *e, X86 *c) { S(e)->claimed = 1; LOGV("[audio] Claim\n"); E_RET(0); }
static void au_UnClaim(Emu *e, X86 *c) { S(e)->claimed = 0; LOGV("[audio] UnClaim\n"); E_RET(0); }
static void au_Start(Emu *e, X86 *c) {
    s4_engine *s = S(e);
    s->started = 1; s->start_notified = 0; s->stop_notified = 0; s->bytes_since_start = 0;
    LOGV("[audio] Start\n");
    E_RET(0);
}
static void au_Stop(Emu *e, X86 *c) { S(e)->started = 0; LOGV("[audio] Stop\n"); E_RET(0); }
static void au_ToFileTime(Emu *e, X86 *c) { (void)e; if (A(2)) wr64(c, A(2), 0); E_RET(0); }
static void au_WaveFormatGet(Emu *e, X86 *c) { (void)e; E_RET(0x80004001u); }
static void au_WaveFormatSet(Emu *e, X86 *c) {
    s4_engine *s = S(e);
    uint32_t p = A(1), n = A(2);
    if (n < 16) { E_RET(0x80070057u); return; }
    uint16_t tag = rd16(c, p), ch = rd16(c, p + 2), bits = rd16(c, p + 14);
    uint32_t rate = rd32(c, p + 4);
    LOGV("[audio] WaveFormatSet tag=%u ch=%u rate=%u bits=%u\n", tag, ch, rate, bits);
    if (tag != 1 || bits != 16 || ch != 1) { E_RET(0x80004005u); return; }
    s->rate = rate; s->channels = ch; s->bits = bits; s->format_set = 1;
    E_RET(0);
}
static void ad_FreeSpace(Emu *e, X86 *c) {
    (void)e;
    if (A(1)) wr32(c, A(1), 0x100000);
    if (A(2)) wr32(c, A(2), 0);
    E_RET(0);
}
static void ad_DataSet(Emu *e, X86 *c) {
    s4_engine *s = S(e);
    uint32_t p = A(1), n = A(2);
    s->total_bytes += n;
    s->bytes_since_start += n;
    s->stop_notified = 0;
    E_RET(0);
    if (!n || s->stopped || !s->fn || !s->speaking) return;
    const int16_t *pcm = (const int16_t *)emu_ptr(e, p, n);
    if (s->fn(pcm, n / 2, s->user)) {
        /* stop: this call still succeeds, then the thread yields and emu_pump returns to s4_speak,
         * which resets the engine */
        s->stopped = 1;
        x86_ret(c, 12);
        e->retry = 1;
        e->stop_reason = STOP_YIELD;
        c->stop = 1;
        emu_abort_pump(e);
    }
}
static void ad_BookMark(Emu *e, X86 *c) {
    s4_engine *s = S(e);
    if (s->nbook < 1024) s->bookmarks[s->nbook++] = A(1);
    E_RET(0);
}

/* ------------------------------------------------------------------ notification sinks */
static void ns_QI(Emu *e, X86 *c) {
    uint32_t riid = A(1), ppv = A(2);
    int ok = guid_eq(e, riid, &IID_IUnknown_) || guid_eq(e, riid, &IID_ITTSNotifySinkA_) || guid_eq(e, riid, &IID_ITTSNotifySinkW_) ||
             guid_eq(e, riid, &IID_ITTSBufNotifySink_);
    wr32(c, ppv, ok ? A(0) : 0);
    E_RET(ok ? 0 : 0x80004002u);
}
static void ns_ref(Emu *e, X86 *c) { (void)e; E_RET(1); }
static void ns_ok(Emu *e, X86 *c) { (void)e; E_RET(0); }
static void ns_AudioStop(Emu *e, X86 *c) { S(e)->audio_stop_seen = 1; LOGV("[tts] AudioStop\n"); E_RET(0); }
static void bs_TextDataDone(Emu *e, X86 *c) { S(e)->textdata_done_seen = 1; LOGV("[tts] TextDataDone\n"); E_RET(0); }

/* idle hook: deliver the device's notifications the way an instantly-playing device would */
static int audio_idle(Emu *e, void *ctx) {
    s4_engine *s = ctx;
    if (!s->sink) return 0;
    if (s->started && !s->start_notified && s->bytes_since_start) {
        s->start_notified = 1;
        emu_com(e, s->sink, 4, 0, NULL);           /* IAudioDestNotifySink::AudioStart */
        return 1;
    }
    if (s->nbook) {
        uint32_t b = s->bookmarks[0];
        memmove(s->bookmarks, s->bookmarks + 1, sizeof(uint32_t) * (size_t)(--s->nbook));
        uint32_t args[2] = { b, 0 };
        emu_com(e, s->sink, 6, 2, args);           /* IAudioDestNotifySink::BookMark(id, FALSE) */
        return 1;
    }
    if (s->started && s->start_notified && !s->stop_notified) {
        s->stop_notified = 1;
        uint32_t args[1] = { 0 };
        emu_com(e, s->sink, 3, 1, args);           /* IAudioDestNotifySink::AudioStop(0) */
        return 1;
    }
    return 0;
}

static const EmuMethod audio_m[] = {
    { "IAudio::QueryInterface", au_QI, 2 }, { "IAudio::AddRef", au_AddRef, 0 }, { "IAudio::Release", au_Release, 0 },
    { "IAudio::Flush", au_Flush, 0 }, { "IAudio::LevelGet", au_LevelGet, 1 }, { "IAudio::LevelSet", au_ok, 1 },
    { "IAudio::PassNotify", au_PassNotify, 5 }, { "IAudio::PosnGet", au_PosnGet, 1 }, { "IAudio::Claim", au_Claim, 0 },
    { "IAudio::UnClaim", au_UnClaim, 0 }, { "IAudio::Start", au_Start, 0 }, { "IAudio::Stop", au_Stop, 0 },
    { "IAudio::TotalGet", au_TotalGet, 1 }, { "IAudio::ToFileTime", au_ToFileTime, 2 },
    { "IAudio::WaveFormatGet", au_WaveFormatGet, 1 }, { "IAudio::WaveFormatSet", au_WaveFormatSet, 2 },
};
static const EmuMethod dest_m[] = {
    { "IAudioDest::QueryInterface", au_QI, 2 }, { "IAudioDest::AddRef", au_AddRef, 0 }, { "IAudioDest::Release", au_Release, 0 },
    { "IAudioDest::FreeSpace", ad_FreeSpace, 2 }, { "IAudioDest::DataSet", ad_DataSet, 2 }, { "IAudioDest::BookMark", ad_BookMark, 1 },
};
static const EmuMethod ns_m[] = {
    { "ITTSNotifySink::QueryInterface", ns_QI, 2 }, { "ITTSNotifySink::AddRef", ns_ref, 0 }, { "ITTSNotifySink::Release", ns_ref, 0 },
    { "ITTSNotifySink::AttribChanged", ns_ok, 1 }, { "ITTSNotifySink::AudioStart", ns_ok, 2 },
    { "ITTSNotifySink::AudioStop", ns_AudioStop, 2 }, { "ITTSNotifySink::Visual", ns_ok, 6 },
};
static const EmuMethod bs_m[] = {
    { "ITTSBufNotifySink::QueryInterface", ns_QI, 2 }, { "ITTSBufNotifySink::AddRef", ns_ref, 0 }, { "ITTSBufNotifySink::Release", ns_ref, 0 },
    { "ITTSBufNotifySink::TextDataDone", bs_TextDataDone, 3 }, { "ITTSBufNotifySink::TextDataStarted", ns_ok, 2 },
    { "ITTSBufNotifySink::BookMark", ns_ok, 3 }, { "ITTSBufNotifySink::WordPosition", ns_ok, 3 },
};

/* ------------------------------------------------------------------ open / close */
static void copy_field(char *dst, size_t n, const char *src, size_t srcmax) {
    size_t k = 0;
    while (k + 1 < n && k < srcmax && src[k]) { dst[k] = src[k]; k++; }
    dst[k] = 0;
}

/* Loads the DLL, creates the host objects and enumerates the modes; selects `mode_name` unless NULL.
 * Must be called with a fault handler installed (c->fault_jmp). */
/* L&H TruVoice: the real MSVCP50.dll (its C++ runtime) as a second guest module, then the engine,
 * installed the way its setup did it (C:\\WINDOWS\\lhsp\\tv, DllRegisterServer into the in-memory
 * registry). The engine runs its own message loops, so it gets per-thread message queues. */
static int truvoice_load(s4_engine *s, const char *dir) {
    Emu *e = s->e;
    char host[1100];
    emu_enable_thread_queues(e);
    emu_map_dir(e, "C:\\WINDOWS\\LHSP\\TV", dir);
    emu_map_dir(e, "C:\\WINDOWS\\SYSTEM", dir);
    if (!emu_host_path(e, "C:\\WINDOWS\\SYSTEM\\msvcp50.dll", host, sizeof host, 1)) { set_err(s->error, sizeof s->error, "msvcp50.dll not found in %s", dir); return -1; }
    EmuModule *rt = emu_load_pe_ex(e, host, "C:\\WINDOWS\\SYSTEM\\MSVCP50.DLL", EMU_IMAGE_BASE + 0x01000000u, EMU_LOAD_NO_CODE_CACHE);
    if (!rt) { set_err(s->error, sizeof s->error, "msvcp50.dll: %s", emu_error(e)); return -1; }
    if (!emu_dll_attach(e, rt)) { set_err(s->error, sizeof s->error, "msvcp50.dll DllMain failed: %s", emu_error(e)); return -1; }
    if (!emu_host_path(e, "C:\\WINDOWS\\LHSP\\TV\\tv_enua.dll", host, sizeof host, 1)) { set_err(s->error, sizeof s->error, "tv_enua.dll not found in %s", dir); return -1; }
    s->mod = emu_load_pe(e, host, "C:\\WINDOWS\\lhsp\\tv\\tv_enua.dll", EMU_IMAGE_BASE);
    if (!s->mod) { set_err(s->error, sizeof s->error, "load failed: %s", emu_error(e)); return -1; }
    if (!emu_dll_attach(e, s->mod)) { set_err(s->error, sizeof s->error, "DllMain failed: %s", emu_error(e)); return -1; }
    uint32_t reg = emu_get_export(e, s->mod, "DllRegisterServer");
    uint32_t hr = reg ? emu_call(e, reg, 0, NULL) : 0x80004005u;
    if (hr || emu_failed(e)) { snprintf(s->error, sizeof s->error, "DllRegisterServer failed hr=%08x %s", hr, emu_error(e)); return -1; }
    return 0;
}

static int engine_init(s4_engine *s, const char *dir, const char *mode_name) {
    Emu *e = s->e;
    X86 *c = emu_cpu(e);
    char host[1100];
    emu_map_dir(e, "C:\\SPEECH", dir);
    if (emu_host_path(e, "C:\\SPEECH\\tv_enua.dll", host, sizeof host, 1)) {
        s->kind = S4_ENGINE_TRUVOICE;
        if (truvoice_load(s, dir)) return -1;
    } else {
        s->kind = S4_ENGINE_MSTTS;
        if (!emu_host_path(e, "C:\\SPEECH\\msttssyn.dll", host, sizeof host, 1)) { set_err(s->error, sizeof s->error, "msttssyn.dll not found in %s", dir); return -1; }
        s->mod = emu_load_pe(e, host, "C:\\SPEECH\\msttssyn.dll", EMU_IMAGE_BASE);
        if (!s->mod) { set_err(s->error, sizeof s->error, "load failed: %s", emu_error(e)); return -1; }
        if (!emu_dll_attach(e, s->mod)) { set_err(s->error, sizeof s->error, "DllMain failed: %s", emu_error(e)); return -1; }
    }

    s->audio_obj = emu_static(e, 16);
    wr32(c, s->audio_obj, emu_vtable(e, audio_m, 16));
    wr32(c, s->audio_obj + 4, emu_vtable(e, dest_m, 6));
    s->refs = 1;
    s->tts_sink = emu_static(e, 8);
    wr32(c, s->tts_sink, emu_vtable(e, ns_m, 7));
    s->buf_sink = emu_static(e, 8);
    wr32(c, s->buf_sink, emu_vtable(e, bs_m, 7));
    emu_set_idle_hook(e, audio_idle, s);

    uint32_t dgco = emu_get_export(e, s->mod, "DllGetClassObject");
    if (!dgco) { set_err(s->error, sizeof s->error, "no DllGetClassObject%s", NULL); return -1; }
    s->out_ptr = emu_static(e, 16);
    uint32_t args[16];
    args[0] = guid_put(e, s->kind == S4_ENGINE_TRUVOICE ? &CLSID_TRUVOICE_ : &CLSID_MSTTS_); args[1] = guid_put(e, &IID_IClassFactory_); args[2] = s->out_ptr;
    uint32_t hr = emu_call(e, dgco, 3, args);
    if (hr || emu_failed(e)) { set_err(s->error, sizeof s->error, "DllGetClassObject failed %s", emu_error(e)); return -1; }
    uint32_t cf = rd32(c, s->out_ptr);
    args[0] = 0; args[1] = guid_put(e, &IID_ITTSEnumA_); args[2] = s->out_ptr;
    hr = emu_com(e, cf, CF_CreateInstance, 3, args);
    if (hr || emu_failed(e)) { set_err(s->error, sizeof s->error, "CreateInstance(ITTSEnum) failed %s", emu_error(e)); return -1; }
    s->enumr = rd32(c, s->out_ptr);

    /* TTSMODEINFOA (1424 bytes): gEngineID@0 szMfgName@16 szProductName@278 gModeID@540 szModeName@556
     * language@818 szSpeaker@884 szStyle@1146 wGender@1408 wAge@1410 dwFeatures@1412 */
    uint32_t mi = emu_static(e, 4096), fetched = emu_static(e, 4);
    int have = 0;
    for (int i = 0; i < 64; i++) {
        args[0] = 1; args[1] = mi; args[2] = fetched;
        hr = emu_com(e, s->enumr, EN_Next, 3, args);
        if (hr || emu_failed(e) || rd32(c, fetched) == 0) break;
        s4_mode m;
        memset(&m, 0, sizeof m);
        copy_field(m.name, sizeof m.name, (const char *)emu_ptr(e, mi + 556, 262), 262);
        copy_field(m.speaker, sizeof m.speaker, (const char *)emu_ptr(e, mi + 884, 262), 262);
        m.gender = rd16(c, mi + 1408);
        m.features = rd32(c, mi + 1412);
        memcpy(m.mode_id, emu_ptr(e, mi + 540, 16), 16);
        if (s->list && s->nlist < s->maxlist) s->list[s->nlist++] = m;
        if (mode_name && !have && !strcmp(m.name, mode_name)) { s->mode = m; have = 1; }
    }
    if (emu_failed(e)) { set_err(s->error, sizeof s->error, "enumeration failed: %s", emu_error(e)); return -1; }
    if (!mode_name) return 0;
    if (!have) { set_err(s->error, sizeof s->error, "mode '%s' not found", mode_name); return -1; }

    /* ITTSEnum::Select(GUID by value, &central, audio object) */
    uint32_t g4[4];
    memcpy(g4, s->mode.mode_id, 16);
    args[0] = g4[0]; args[1] = g4[1]; args[2] = g4[2]; args[3] = g4[3]; args[4] = s->out_ptr; args[5] = s->audio_obj;
    hr = emu_com(e, s->enumr, EN_Select, 6, args);
    if (hr || emu_failed(e)) { set_err(s->error, sizeof s->error, "Select failed %s", emu_error(e)); return -1; }
    s->central = rd32(c, s->out_ptr);
    emu_pump(e);
    if (emu_failed(e)) { set_err(s->error, sizeof s->error, "Select: %s", emu_error(e)); return -1; }

    args[0] = guid_put(e, &IID_ITTSAttributesA_); args[1] = s->out_ptr;
    hr = emu_com(e, s->central, TC_QueryInterface, 2, args);
    s->attrs = hr ? 0 : rd32(c, s->out_ptr);
    s->scratch = emu_static(e, 8);
    if (s->attrs) {
        args[0] = s->scratch; emu_com(e, s->attrs, TA_PitchGet, 1, args); s->lim.pitch_default = rd16(c, s->scratch);
        args[0] = s->scratch; emu_com(e, s->attrs, TA_SpeedGet, 1, args); s->lim.speed_default = rd32(c, s->scratch);
    }
    s->pitch_cur = s->lim.pitch_default;
    s->speed_cur = s->lim.speed_default;

    /* register the notification sink (ITTSCentral::Register(sink, IID by value, &key)) */
    args[0] = s->tts_sink;
    memcpy(&args[1], &IID_ITTSNotifySinkA_, 16);
    args[5] = s->scratch;
    emu_com(e, s->central, TC_Register, 6, args);
    if (emu_failed(e)) { set_err(s->error, sizeof s->error, "Register: %s", emu_error(e)); return -1; }

    /* pitch / speed ranges: TTSATTR_MIN* / MAX* set the engine's own limits; read them back, restore */
    if (s->attrs) {
        args[0] = 0; emu_com(e, s->attrs, TA_PitchSet, 1, args);
        args[0] = s->scratch; emu_com(e, s->attrs, TA_PitchGet, 1, args); s->lim.pitch_min = rd16(c, s->scratch);
        args[0] = 0xFFFF; emu_com(e, s->attrs, TA_PitchSet, 1, args);
        args[0] = s->scratch; emu_com(e, s->attrs, TA_PitchGet, 1, args); s->lim.pitch_max = rd16(c, s->scratch);
        args[0] = s->lim.pitch_default; emu_com(e, s->attrs, TA_PitchSet, 1, args);
        args[0] = 0; emu_com(e, s->attrs, TA_SpeedSet, 1, args);
        args[0] = s->scratch; emu_com(e, s->attrs, TA_SpeedGet, 1, args); s->lim.speed_min = rd32(c, s->scratch);
        args[0] = 0xFFFFFFFFu; emu_com(e, s->attrs, TA_SpeedSet, 1, args);
        args[0] = s->scratch; emu_com(e, s->attrs, TA_SpeedGet, 1, args); s->lim.speed_max = rd32(c, s->scratch);
        args[0] = s->lim.speed_default; emu_com(e, s->attrs, TA_SpeedSet, 1, args);
        emu_pump(e);
        if (emu_failed(e)) { set_err(s->error, sizeof s->error, "attributes: %s", emu_error(e)); return -1; }
    }
    return 0;
}

static s4_engine *engine_new(const char *dir, const char *mode_name, s4_mode *list, int maxlist, char *err, size_t errlen) {
    s4_engine *s = calloc(1, sizeof *s);
    if (!s) { set_err(err, errlen, "out of memory%s", NULL); return NULL; }
    s->list = list;
    s->maxlist = maxlist;
#ifndef CV_NO_DEBUG_ENV
    s->verbose = getenv("SAPI4_VERBOSE") != NULL;
#endif
    s->e = emu_create(MEM_SIZE);
    if (!s->e) { free(s); set_err(err, errlen, "cannot create the emulator%s", NULL); return NULL; }
    emu_set_user(s->e, s);
#ifdef X86_LOCKSTEP
    emu_lockstep_enable(s->e);
#endif
    X86 *c = emu_cpu(s->e);
    jmp_buf jb;
    c->fault_jmp = &jb;
    int rc;
    if (setjmp(jb)) {
        c->fault_jmp = NULL;
        snprintf(s->error, sizeof s->error, "guest fault: %s", c->fault_msg);
        rc = -1;
    } else {
        rc = engine_init(s, dir, mode_name);
        c->fault_jmp = NULL;
    }
    if (rc) {
        set_err(err, errlen, "%s", s->error);
        emu_destroy(s->e);
        free(s);
        return NULL;
    }
    s->list = NULL;
    return s;
}

s4_engine *s4_open(const char *dir, const char *mode_name, char *err, size_t errlen) {
    if (!dir || !mode_name) { set_err(err, errlen, "bad arguments%s", NULL); return NULL; }
    return engine_new(dir, mode_name, NULL, 0, err, errlen);
}

int s4_list_modes(const char *dir, s4_mode *modes, int max, char *err, size_t errlen) {
    if (!dir) return -1;
    s4_engine *s = engine_new(dir, NULL, modes, max, err, errlen);
    if (!s) return -1;
    int n = s->nlist;
    s4_close(s);
    return n;
}

void s4_close(s4_engine *s) {
    if (!s) return;
    emu_destroy(s->e);
    free(s);
}

int s4_sample_rate(const s4_engine *s) { return s && s->rate ? (int)s->rate : 22050; }
int s4_engine_kind(const s4_engine *s) { return s ? s->kind : 0; }
const s4_mode *s4_mode_info(const s4_engine *s) { return s ? &s->mode : NULL; }
void s4_get_limits(const s4_engine *s, s4_limits *out) { if (s && out) *out = s->lim; }
const char *s4_error(const s4_engine *s) { return s ? s->error : "no engine"; }
void s4_last_stats(const s4_engine *s, s4_stats *out) { if (s && out) *out = s->last; }

/* ------------------------------------------------------------------ attributes */
static int set_attr(s4_engine *s, int method, uint32_t v) {
    Emu *e = s->e;
    X86 *c = emu_cpu(e);
    if (s->dead || !s->attrs) return -1;
    jmp_buf jb;
    c->fault_jmp = &jb;
    if (setjmp(jb)) {
        c->fault_jmp = NULL;
        snprintf(s->error, sizeof s->error, "guest fault: %s", c->fault_msg);
        s->dead = 1;
        return -1;
    }
    uint32_t args[1] = { v };
    uint32_t hr = emu_com(e, s->attrs, method, 1, args);
    emu_pump(e);
    c->fault_jmp = NULL;
    if (emu_failed(e)) { snprintf(s->error, sizeof s->error, "%s", emu_error(e)); s->dead = 1; return -1; }
    return hr ? -1 : 0;
}

int s4_set_pitch(s4_engine *s, unsigned pitch) {
    if (!s) return -1;
    if (pitch < s->lim.pitch_min) pitch = s->lim.pitch_min;
    if (pitch > s->lim.pitch_max) pitch = s->lim.pitch_max;
    if (pitch == s->pitch_cur) return 0;
    if (set_attr(s, TA_PitchSet, pitch)) return -1;
    s->pitch_cur = pitch;
    return 0;
}

int s4_set_speed(s4_engine *s, unsigned wpm) {
    if (!s) return -1;
    if (wpm < s->lim.speed_min) wpm = s->lim.speed_min;
    if (wpm > s->lim.speed_max) wpm = s->lim.speed_max;
    if (wpm == s->speed_cur) return 0;
    if (set_attr(s, TA_SpeedSet, wpm)) return -1;
    s->speed_cur = wpm;
    return 0;
}

/* ------------------------------------------------------------------ speaking */
int s4_speak(s4_engine *s, const char *text, int flags, s4_pcm_fn fn, void *user) {
    if (!s || !text) return -1;
    if (s->dead || emu_failed(s->e)) return -1;
    size_t len = strlen(text);
    if (!len) return 0;
    if (len > 1000000) len = 1000000;
    Emu *e = s->e;
    X86 *c = emu_cpu(e);
    uint64_t i0 = c->icount;
    EmuStats st0 = emu_stats(e);
    uint32_t gtext = 0;
    int rc = 0;

    s->fn = fn;
    s->user = user;
    s->stopped = 0;
    s->speaking = 1;
    s->audio_stop_seen = 0;
    s->textdata_done_seen = 0;
    emu_clear_abort(e);

    jmp_buf jb;
    c->fault_jmp = &jb;
    if (setjmp(jb)) {
        c->fault_jmp = NULL;
        snprintf(s->error, sizeof s->error, "guest fault: %s", c->fault_msg);
        s->dead = 1;
        s->speaking = 0;
        s->fn = NULL;
        return -1;
    }
    /* watchdog: generous (about 20x what the slowest settings need), it only exists to turn a guest
     * hang into an error instead of a stuck thread */
    emu_set_insn_budget(e, 2000000000ull + 400000000ull * (uint64_t)len);
    uint32_t tlen = (uint32_t)len + 1;
    gtext = emu_malloc(e, tlen);
    memcpy(emu_ptr(e, gtext, tlen), text, len);
    ((char *)emu_ptr(e, gtext, tlen))[len] = 0;

    /* TextData(CHARSET_TEXT, flags, SDATA{text, len+1} by value, buffer sink, IID by value) */
    uint32_t args[16];
    /* ITTSBufNotifySink: Microsoft's engine gets one (its output is identical either way). TruVoice gets
     * none: with a buffer sink it tracks word positions on its synthesis thread and its audio changes;
     * without one it is bit-identical to the genuine engine as tetyys.com runs it (no sink either). */
    uint32_t bufsink = s->kind == S4_ENGINE_TRUVOICE ? 0 : s->buf_sink;
#ifndef CV_NO_DEBUG_ENV
    { const char *v = getenv("SAPI4_BUFSINK"); if (v) bufsink = *v == '1' ? s->buf_sink : 0; }        /* debug A/B */
    { const char *v = getenv("SAPI4_RESET_FIRST"); if (v && *v == '1') emu_com(e, s->central, TC_AudioReset, 0, NULL); }
#endif
    args[0] = 0; args[1] = (uint32_t)(flags & S4_TAGGED); args[2] = gtext; args[3] = tlen; args[4] = bufsink;
    memcpy(&args[5], &IID_ITTSBufNotifySink_, 16);
    uint32_t hr = emu_com(e, s->central, TC_TextData, 9, args);
    if (hr || emu_failed(e)) {
        snprintf(s->error, sizeof s->error, "TextData hr=%08x %s", hr, emu_error(e));
        rc = -1;
    } else {
        emu_pump(e);
        if (emu_aborted(e) && !emu_failed(e)) {
            /* stopped by the caller: flush the engine (ITTSCentral::AudioReset) and let it settle */
            emu_clear_abort(e);
            emu_set_insn_budget(e, 4000000000ull);
            emu_com(e, s->central, TC_AudioReset, 0, NULL);
            emu_pump(e);
            emu_clear_abort(e);
        }
        if (emu_failed(e)) { snprintf(s->error, sizeof s->error, "%s", emu_error(e)); rc = -1; }
        else rc = s->stopped ? 1 : 0;
    }
    if (!emu_failed(e)) emu_free(e, gtext);
    emu_set_insn_budget(e, 0);
    c->fault_jmp = NULL;
    if (rc < 0) s->dead = 1;
    s->speaking = 0;
    s->fn = NULL;
    s->user = NULL;
    EmuStats st1 = emu_stats(e);
    s->last.insns = c->icount - i0;
    s->last.host_calls = st1.host_calls - st0.host_calls;
    s->last.heap_live = e->heap_live;
    s->last.heap_top = e->heap_next;
    s->last.audio_stop_seen = s->audio_stop_seen;
    s->last.textdata_done_seen = s->textdata_done_seen;
    return rc;
}
