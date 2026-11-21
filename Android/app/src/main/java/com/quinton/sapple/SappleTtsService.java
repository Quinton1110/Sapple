package com.quinton.sapple;

import android.content.SharedPreferences;
import android.media.AudioFormat;
import android.speech.tts.SynthesisCallback;
import android.speech.tts.SynthesisRequest;
import android.speech.tts.TextToSpeech;
import android.speech.tts.TextToSpeechService;
import android.speech.tts.Voice;

import java.util.ArrayList;
import java.util.HashSet;
import java.util.List;
import java.util.Locale;
import java.util.Set;

/** Sapple as an Android text-to-speech engine: what TalkBack and every other app that speaks talk to.
 *
 *  Android asks for a language more often than a voice (TalkBack does), so the voice used for a plain language
 *  request is the one picked in the app; an app that asks for a named voice gets that voice. The engines speak
 *  English only, so every voice is offered under its own English locale and any other English request falls back to
 *  the chosen voice. Nothing here logs or stores the text it speaks. */
public class SappleTtsService extends TextToSpeechService {
    static final String PREFS = "sapple";
    static final String KEY_VOICE = "voice";

    private VoiceData data;
    private volatile boolean stopRequested;

    @Override
    public void onCreate() {
        data = new VoiceData(this);
        super.onCreate();
    }

    static Voices.Def chosenVoice(android.content.Context ctx, VoiceData data) {
        SharedPreferences p = ctx.getSharedPreferences(PREFS, MODE_PRIVATE);
        Voices.Def d = Voices.bySlug(p.getString(KEY_VOICE, "sam"));
        if (d != null && data.has(d)) return d;
        for (Voices.Def v : Voices.ALL) if (data.has(v)) return v;
        return null;
    }

    // ---- languages ----

    @Override
    protected int onIsLanguageAvailable(String lang, String country, String variant) {
        if (!isEnglish(lang)) return TextToSpeech.LANG_NOT_SUPPORTED;
        if (data.availableCount() == 0) return TextToSpeech.LANG_MISSING_DATA;
        if (country == null || country.isEmpty()) return TextToSpeech.LANG_AVAILABLE;
        for (Voices.Def d : Voices.ALL) {
            if (data.has(d) && d.locale.getISO3Country().equalsIgnoreCase(iso3Country(country)))
                return TextToSpeech.LANG_COUNTRY_AVAILABLE;
        }
        return TextToSpeech.LANG_AVAILABLE;
    }

    @Override
    protected String[] onGetLanguage() {
        Voices.Def d = chosenVoice(this, data);
        Locale l = d != null ? d.locale : Locale.US;
        return new String[] {l.getISO3Language(), l.getISO3Country(), ""};
    }

    @Override
    protected int onLoadLanguage(String lang, String country, String variant) {
        return onIsLanguageAvailable(lang, country, variant);
    }

    // ---- voices ----

    @Override
    public List<Voice> onGetVoices() {
        List<Voice> out = new ArrayList<>();
        for (Voices.Def d : Voices.ALL) {
            if (!data.has(d)) continue;
            Set<String> features = new HashSet<>();
            out.add(new Voice(d.slug, d.locale, Voice.QUALITY_NORMAL, Voice.LATENCY_VERY_LOW, false, features));
        }
        return out;
    }

    @Override
    public int onIsValidVoiceName(String name) {
        Voices.Def d = Voices.bySlug(name);
        return d != null && data.has(d) ? TextToSpeech.SUCCESS : TextToSpeech.ERROR;
    }

    @Override
    public int onLoadVoice(String name) {
        if (onIsValidVoiceName(name) != TextToSpeech.SUCCESS) return TextToSpeech.ERROR;
        return TextToSpeech.SUCCESS;
    }

    @Override
    public String onGetDefaultVoiceNameFor(String lang, String country, String variant) {
        if (!isEnglish(lang)) return null;
        Voices.Def chosen = chosenVoice(this, data);
        return chosen != null ? chosen.slug : null;
    }

    // ---- speaking ----

    @Override
    protected void onStop() {
        stopRequested = true;
    }

    @Override
    protected void onSynthesizeText(SynthesisRequest request, SynthesisCallback callback) {
        stopRequested = false;
        Voices.Def def = Voices.bySlug(request.getVoiceName());
        if (def == null || !data.has(def)) {
            // A language request (TalkBack): the voice picked in the app.
            def = chosenVoice(this, data);
        }
        if (def == null) {
            callback.error(TextToSpeech.ERROR_NOT_INSTALLED_YET);
            return;
        }
        CharSequence cs = request.getCharSequenceText();
        String text = cs != null ? cs.toString() : "";
        if (callback.start(Engine.SAMPLE_RATE, AudioFormat.ENCODING_PCM_16BIT, 1) != TextToSpeech.SUCCESS) return;
        if (text.trim().isEmpty()) {
            callback.done();
            return;
        }
        final SynthesisCallback cb = callback;
        String[] err = new String[1];
        int rc = Engine.speak(data, def, text, Engine.sapiRateFor(request.getSpeechRate()),
                Engine.semitonesFor(request.getPitch()), callback.getMaxBufferSize(),
                (buf, len) -> !stopRequested && cb.audioAvailable(buf, 0, len) == TextToSpeech.SUCCESS, err);
        if (rc < 0 && !stopRequested) callback.error(TextToSpeech.ERROR_SYNTHESIS);
        else callback.done();
    }

    private static boolean isEnglish(String lang) {
        if (lang == null) return false;
        String l = lang.toLowerCase(Locale.ROOT);
        return l.equals("eng") || l.equals("en");
    }

    private static String iso3Country(String c) {
        if (c.length() == 3) return c;
        try {
            return new Locale("", c).getISO3Country();
        } catch (Exception e) {
            return c;
        }
    }
}
