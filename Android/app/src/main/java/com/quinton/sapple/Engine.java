package com.quinton.sapple;

import java.util.ArrayList;
import java.util.List;

/** The Java side of the C engines: a small pool of voice handles, as in ClassicEngine.swift. Handles are not
 *  reentrant, so every utterance checks one out and returns it afterwards; a few stay warm for the next one. */
final class Engine {
    static final int SAPI5 = 0, SAPI4 = 1, TRUVOICE = 2, ANNA = 3, ONECORE = 4, NEURAL = 5;   // must match sapple_jni.c
    static final int SAMPLE_RATE = 22050;

    static {
        System.loadLibrary("sapple");
    }

    static native boolean nativeHasTruVoice();
    private static native long nativeOpen(int kind, String dataDir, String name, String effect, double basePitch,
                                          String[] err);
    private static native long nativeOpenNeural(String sdkDir, String dataDir, String onecoreDir, String workDir,
                                                String name, String[] err);
    private static native void nativeClose(long handle);
    private static native int nativeSpeak(long handle, String text, double sapiRate, double semitones, String emotion,
                                          PcmSink sink, byte[] buf);

    /** Receives 16-bit little-endian mono PCM at 22050 Hz; return false to stop. */
    interface PcmSink {
        boolean onPcm(byte[] buf, int length);
    }

    private static final class Idle {
        final String key;
        final long handle;
        Idle(String key, long handle) { this.key = key; this.handle = handle; }
    }

    private static final Object lock = new Object();
    private static final List<Idle> idle = new ArrayList<>();
    private static final int MAX_IDLE = 3;

    /** Speaks plain text. Returns 0 done, 1 stopped by the sink, -1 error; err[0] gets a reason on an open failure. */
    static int speak(VoiceData data, Voices.Def def, String text, double sapiRate, double semitones, int chunkBytes,
                     PcmSink sink, String[] err) {
        long h = checkout(data, def, err);
        if (h == 0) {
            Diagnostics.recordFailure(def, err != null && err.length > 0 ? err[0] : null);
            return -1;
        }
        byte[] buf = new byte[Math.max(2, chunkBytes & ~1)];
        int rc = nativeSpeak(h, text, sapiRate, semitones, def.emotion, sink, buf);
        if (rc < 0) {
            nativeClose(h);
            Diagnostics.recordFailure(def, "the voice stopped with an error while speaking");
        }   // a failed handle is never handed out again (as on iOS)
        else checkin(def, h);
        return rc;
    }

    private static long checkout(VoiceData data, Voices.Def def, String[] err) {
        String key = def.poolKey();
        synchronized (lock) {
            for (int i = idle.size() - 1; i >= 0; i--) {
                if (idle.get(i).key.equals(key)) return idle.remove(i).handle;
            }
        }
        if (def.engine == NEURAL) {
            return nativeOpenNeural(data.sdkDir(), data.dirFor(NEURAL), data.dirFor(ONECORE), data.neuralWorkDir(),
                                    def.name, err);
        }
        return nativeOpen(def.engine, data.dirFor(def.engine), def.name, def.effect, def.basePitch, err);
    }

    private static void checkin(Voices.Def def, long h) {
        List<Long> evicted = new ArrayList<>();
        synchronized (lock) {
            idle.add(new Idle(def.poolKey(), h));
            while (idle.size() > MAX_IDLE) evicted.add(idle.remove(0).handle);
        }
        for (long e : evicted) nativeClose(e);
    }

    /** Closes every idle handle (after the voice data changes, so nothing keeps reading the old files). */
    static void releaseIdle() {
        List<Long> all = new ArrayList<>();
        synchronized (lock) {
            for (Idle i : idle) all.add(i.handle);
            idle.clear();
        }
        for (long h : all) nativeClose(h);
    }

    /** SAPI rate (-10..18, speed factor 3^(rate/10)) for an Android speech rate (100 = normal). */
    static double sapiRateFor(int androidRate) {
        double r = Math.max(10, androidRate) / 100.0;
        double s = 10.0 * Math.log(r) / Math.log(3.0);
        return Math.max(-10, Math.min(18, s));
    }

    /** Semitones (-12..12) for an Android pitch (100 = normal). */
    static double semitonesFor(int androidPitch) {
        double p = Math.max(10, androidPitch) / 100.0;
        double s = 12.0 * Math.log(p) / Math.log(2.0);
        return Math.max(-12, Math.min(12, s));
    }

    private Engine() {}
}
