/* Sapple for Android: JNI over the same C bridges the iOS / macOS extension uses (Shared/Bridge). Every bridge has
 * one shape - open, speak(text, SAPI rate, semitones, trim, PCM callback), close - and streams 22050 Hz 16-bit mono
 * PCM, which is exactly what an Android TextToSpeechService hands to its SynthesisCallback.
 *
 * A handle is a small struct naming its engine. Handles are not reentrant (same rule as ClassicEngine.swift): the Java
 * side pools them and gives each utterance its own. Nothing here logs or stores the text it is given. */
#include <jni.h>
#include <stdlib.h>
#include <string.h>

#include "cv_bridge.h"
#include "cv4n_bridge.h"
#include "cva_bridge.h"
#include "cvo_bridge.h"
#include "cvn_bridge.h"
#if SAPPLE_HAVE_TRUVOICE
#include "cvt_bridge.h"
#endif

/* Must match Engine.java. */
enum { K_SAPI5 = 0, K_SAPI4 = 1, K_TRUVOICE = 2, K_ANNA = 3, K_ONECORE = 4, K_NEURAL = 5 };

typedef struct {
    int kind;
    void *v;
} handle;

typedef struct {
    JNIEnv *env;
    jobject sink;
    jmethodID on_pcm;
    jbyteArray buf;
    jint cap;      /* bytes */
    int failed;
} speak_ctx;

static int on_pcm(const int16_t *pcm, size_t n, void *user) {
    speak_ctx *c = user;
    if (c->failed) return 1;
    /* Hand the PCM over in pieces no larger than the reusable Java buffer. */
    while (n > 0) {
        size_t take = n;
        if (take * 2 > (size_t)c->cap) take = (size_t)c->cap / 2;
        (*c->env)->SetByteArrayRegion(c->env, c->buf, 0, (jsize)(take * 2), (const jbyte *)pcm);
        jboolean go = (*c->env)->CallBooleanMethod(c->env, c->sink, c->on_pcm, c->buf, (jint)(take * 2));
        if ((*c->env)->ExceptionCheck(c->env)) {
            c->failed = 1;
            return 1;
        }
        if (!go) return 1;
        pcm += take;
        n -= take;
    }
    return 0;
}

static const char *chars(JNIEnv *env, jstring s) {
    return s ? (*env)->GetStringUTFChars(env, s, NULL) : NULL;
}

static void release(JNIEnv *env, jstring s, const char *c) {
    if (s && c) (*env)->ReleaseStringUTFChars(env, s, c);
}

JNIEXPORT jboolean JNICALL Java_com_quinton_sapple_Engine_nativeHasTruVoice(JNIEnv *env, jclass cls) {
    return SAPPLE_HAVE_TRUVOICE ? JNI_TRUE : JNI_FALSE;
}

/* kind: K_*. dataDir: the engine's data folder (unused by TruVoice, whose tables are compiled in). name: the SAPI 5
 * .spd name, the SAPI 4 / TruVoice mode name or the OneCore voice. effect / basePitch: SAPI 5 only.
 * Returns 0 and leaves the reason in err[0] on failure. */
JNIEXPORT jlong JNICALL Java_com_quinton_sapple_Engine_nativeOpen(JNIEnv *env, jclass cls, jint kind, jstring jdir,
                                                                    jstring jname, jstring jeffect, jdouble basePitch,
                                                                    jobjectArray jerr) {
    char err[256] = {0};
    const char *dir = chars(env, jdir), *name = chars(env, jname), *effect = chars(env, jeffect);
    void *v = NULL;
    switch (kind) {
    case K_SAPI5: v = cv_voice_open(dir, name, effect, basePitch, err, sizeof err); break;
    case K_SAPI4: v = cv4n_voice_open(dir, name, err, sizeof err); break;
#if SAPPLE_HAVE_TRUVOICE
    case K_TRUVOICE: v = cvt_voice_open(name, err, sizeof err); break;
#endif
    case K_ANNA: v = cva_voice_open(dir, err, sizeof err); break;
    case K_ONECORE: v = cvo_voice_open(dir, name, err, sizeof err); break;
    default: strcpy(err, "this engine is not in this build"); break;
    }
    release(env, jdir, dir);
    release(env, jname, name);
    release(env, jeffect, effect);
    if (!v) {
        if (jerr && (*env)->GetArrayLength(env, jerr) > 0) {
            jstring s = (*env)->NewStringUTF(env, err[0] ? err : "could not open the voice");
            (*env)->SetObjectArrayElement(env, jerr, 0, s);
        }
        return 0;
    }
    handle *h = malloc(sizeof *h);
    if (!h) {
        /* Out of memory: close what we opened rather than leak it. */
        switch (kind) {
        case K_SAPI5: cv_voice_close(v); break;
        case K_SAPI4: cv4n_voice_close(v); break;
#if SAPPLE_HAVE_TRUVOICE
        case K_TRUVOICE: cvt_voice_close(v); break;
#endif
        case K_ANNA: cva_voice_close(v); break;
        case K_ONECORE: cvo_voice_close(v); break;
        case K_NEURAL: cvn_voice_close(v); break;
        }
        return 0;
    }
    h->kind = kind;
    h->v = v;
    return (jlong)(intptr_t)h;
}

/* The Windows 11 natural voices (Microsoft's embedded Speech SDK, cvn_bridge.h). sdkDir: the app's native library
 * folder, where the SDK's .so files are. dataDir: the imported NeuralVoices folder. onecoreDir: the imported
 * OneCoreVoice folder (the language data both families share). workDir: a writable folder for the bridge's links. */
JNIEXPORT jlong JNICALL Java_com_quinton_sapple_Engine_nativeOpenNeural(JNIEnv *env, jclass cls, jstring jsdk,
                                                                          jstring jdata, jstring jonecore,
                                                                          jstring jwork, jstring jname,
                                                                          jobjectArray jerr) {
    char err[256] = {0};
    const char *sdk = chars(env, jsdk), *data = chars(env, jdata), *onecore = chars(env, jonecore),
               *work = chars(env, jwork), *name = chars(env, jname);
    cvn_voice *v = cvn_voice_open(sdk, data, onecore, work, name, err, sizeof err);
    release(env, jsdk, sdk);
    release(env, jdata, data);
    release(env, jonecore, onecore);
    release(env, jwork, work);
    release(env, jname, name);
    handle *h = v ? malloc(sizeof *h) : NULL;
    if (!h) {
        if (v) cvn_voice_close(v);
        else if (jerr && (*env)->GetArrayLength(env, jerr) > 0) {
            jstring s = (*env)->NewStringUTF(env, err[0] ? err : "could not open the voice");
            (*env)->SetObjectArrayElement(env, jerr, 0, s);
        }
        return 0;
    }
    h->kind = K_NEURAL;
    h->v = v;
    return (jlong)(intptr_t)h;
}

JNIEXPORT void JNICALL Java_com_quinton_sapple_Engine_nativeClose(JNIEnv *env, jclass cls, jlong jh) {
    handle *h = (handle *)(intptr_t)jh;
    if (!h) return;
    switch (h->kind) {
    case K_SAPI5: cv_voice_close(h->v); break;
    case K_SAPI4: cv4n_voice_close(h->v); break;
#if SAPPLE_HAVE_TRUVOICE
    case K_TRUVOICE: cvt_voice_close(h->v); break;
#endif
    case K_ANNA: cva_voice_close(h->v); break;
    case K_ONECORE: cvo_voice_close(h->v); break;
    case K_NEURAL: cvn_voice_close(h->v); break;
    }
    free(h);
}

/* Speaks plain text, calling sink.onPcm(byte[] buf, int length) with little-endian 16-bit PCM at 22050 Hz; onPcm
 * returns false to stop. emotion: OneCore only ("" = normal). Returns 0 done, 1 stopped, -1 error (close the handle,
 * never reuse it, as on iOS). */
JNIEXPORT jint JNICALL Java_com_quinton_sapple_Engine_nativeSpeak(JNIEnv *env, jclass cls, jlong jh, jstring jtext,
                                                                    jdouble rate, jdouble semitones, jstring jemotion,
                                                                    jobject sink, jbyteArray buf) {
    handle *h = (handle *)(intptr_t)jh;
    if (!h || !jtext || !sink || !buf) return -1;
    jclass sc = (*env)->GetObjectClass(env, sink);
    jmethodID m = (*env)->GetMethodID(env, sc, "onPcm", "([BI)Z");
    if (!m) return -1;
    speak_ctx c = {env, sink, m, buf, (*env)->GetArrayLength(env, buf), 0};
    if (c.cap < 2) return -1;

    /* GetStringUTFChars gives modified UTF-8 (supplementary characters as surrogate pairs); the sanitizer folds or
     * drops what the engines cannot say either way, so that is harmless here. */
    const char *text = chars(env, jtext);
    const char *emotion = chars(env, jemotion);
    int rc = -1;
    switch (h->kind) {
    case K_SAPI5: rc = cv_voice_speak(h->v, text, rate, semitones, 1, on_pcm, &c); break;
    case K_SAPI4: rc = cv4n_voice_speak(h->v, text, rate, semitones, 1, on_pcm, &c); break;
#if SAPPLE_HAVE_TRUVOICE
    case K_TRUVOICE: rc = cvt_voice_speak(h->v, text, rate, semitones, 1, on_pcm, &c); break;
#endif
    case K_ANNA: rc = cva_voice_speak(h->v, text, rate, semitones, 1, on_pcm, &c); break;
    case K_ONECORE:
        cvo_voice_set_emotion(h->v, emotion ? emotion : "");
        rc = cvo_voice_speak(h->v, text, rate, semitones, 1, on_pcm, &c);
        break;
    case K_NEURAL: rc = cvn_voice_speak(h->v, text, rate, semitones, 1, on_pcm, &c); break;
    }
    release(env, jtext, text);
    release(env, jemotion, emotion);
    if (c.failed) return -1;
    return rc;
}
