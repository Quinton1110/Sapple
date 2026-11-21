package com.quinton.sapple;

import static org.junit.Assert.assertEquals;
import static org.junit.Assert.assertNotNull;
import static org.junit.Assert.assertTrue;

import android.content.Context;
import android.speech.tts.TextToSpeech;
import android.speech.tts.UtteranceProgressListener;
import android.speech.tts.Voice;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.RandomAccessFile;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;

/** Speaks through Android's own TextToSpeech API, bound to this engine, exactly as TalkBack would reach it: one voice
 *  per engine, each rendered to a WAV that must hold real audio. Needs voice data on the device (VoiceData.java);
 *  voices whose data is missing are skipped. The WAVs stay in the app's cache (files/tts-test) for listening. */
@RunWith(AndroidJUnit4.class)
public class EngineSpeaksTest {
    private static final String[] VOICES = {"sam", "mike_hall", "sapi4_mary", "anna", "onecore_david",
            "onecore_zira_happy", "onecore_hazel", "onecore_eva", "truvoice_adult_male_1"};

    @Test
    public void everyEngineSpeaks() throws Exception {
        Context ctx = InstrumentationRegistry.getInstrumentation().getTargetContext();
        CountDownLatch ready = new CountDownLatch(1);
        int[] status = {TextToSpeech.ERROR};
        TextToSpeech[] tts = new TextToSpeech[1];
        InstrumentationRegistry.getInstrumentation().runOnMainSync(() ->
                tts[0] = new TextToSpeech(ctx, s -> { status[0] = s; ready.countDown(); }, "com.quinton.sapple"));
        assertTrue(ready.await(20, TimeUnit.SECONDS));
        assertEquals(TextToSpeech.SUCCESS, status[0]);
        File dir = new File(ctx.getFilesDir(), "tts-test");
        dir.mkdirs();
        VoiceData data = new VoiceData(ctx);
        int spoken = 0;
        for (String slug : VOICES) {
            Voices.Def def = Voices.bySlug(slug);
            assertNotNull(slug, def);
            if (!data.has(def)) continue;
            Voice v = null;
            for (Voice x : tts[0].getVoices()) if (x.getName().equals(slug)) v = x;
            assertNotNull("engine lists " + slug, v);
            assertEquals(TextToSpeech.SUCCESS, tts[0].setVoice(v));
            File out = new File(dir, slug + ".wav");
            out.delete();
            CountDownLatch done = new CountDownLatch(1);
            boolean[] ok = {false};
            tts[0].setOnUtteranceProgressListener(new UtteranceProgressListener() {
                @Override public void onStart(String id) {}
                @Override public void onDone(String id) { ok[0] = true; done.countDown(); }
                @Override public void onError(String id) { done.countDown(); }
            });
            tts[0].synthesizeToFile("The quick brown fox jumps over the lazy dog. Testing " + def.display + ".",
                    null, out, slug);
            assertTrue(slug + " finished", done.await(60, TimeUnit.SECONDS));
            assertTrue(slug + " reported done", ok[0]);
            double seconds = (out.length() - 44) / 2.0 / Engine.SAMPLE_RATE;
            assertTrue(slug + " length " + seconds, seconds > 1.5 && seconds < 20);
            assertTrue(slug + " is not silent", rms(out) > 300);
            spoken++;
        }
        tts[0].shutdown();
        assertTrue("at least one voice had data", spoken > 0);
    }

    private static double rms(File wav) throws Exception {
        try (RandomAccessFile f = new RandomAccessFile(wav, "r")) {
            f.seek(44);
            byte[] b = new byte[(int) (f.length() - 44)];
            f.readFully(b);
            double sum = 0;
            int n = b.length / 2;
            for (int i = 0; i < n; i++) {
                short s = (short) ((b[2 * i] & 0xff) | (b[2 * i + 1] << 8));
                sum += (double) s * s;
            }
            return Math.sqrt(sum / Math.max(1, n));
        }
    }
}
