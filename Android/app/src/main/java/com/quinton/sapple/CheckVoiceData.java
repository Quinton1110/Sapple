package com.quinton.sapple;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.speech.tts.TextToSpeech;

import java.util.ArrayList;
import java.util.LinkedHashSet;
import java.util.Set;

/** Answers Android's "is this engine's data installed?" check with the locales that have a voice on the phone. */
public class CheckVoiceData extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        VoiceData data = new VoiceData(this);
        Set<String> have = new LinkedHashSet<>();
        for (Voices.Def d : Voices.ALL) {
            if (data.has(d)) have.add(d.locale.getISO3Language() + "-" + d.locale.getISO3Country());
        }
        Set<String> all = new LinkedHashSet<>();
        for (Voices.Def d : Voices.ALL) all.add(d.locale.getISO3Language() + "-" + d.locale.getISO3Country());
        ArrayList<String> missing = new ArrayList<>();
        for (String l : all) if (!have.contains(l)) missing.add(l);
        Intent r = new Intent();
        r.putStringArrayListExtra(TextToSpeech.Engine.EXTRA_AVAILABLE_VOICES, new ArrayList<>(have));
        r.putStringArrayListExtra(TextToSpeech.Engine.EXTRA_UNAVAILABLE_VOICES, missing);
        setResult(have.isEmpty() ? TextToSpeech.Engine.CHECK_VOICE_DATA_FAIL : TextToSpeech.Engine.CHECK_VOICE_DATA_PASS, r);
        finish();
    }
}
