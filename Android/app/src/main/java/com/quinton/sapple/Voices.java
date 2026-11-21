package com.quinton.sapple;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.Locale;

/** The voice catalog: the same voices, slugs and names as Shared/Voices.swift, minus the neural voices (they run on
 *  Microsoft's embedded Speech SDK, which this build does not include). Keep the two lists in step. */
final class Voices {
    static final class Def {
        final String slug, display;
        final int engine;
        final String name;      // SAPI 5: the .spd name; SAPI 4 / TruVoice: the engine's mode; OneCore: the voice
        final String effect;    // SAPI 5 effect mode, "none" otherwise
        final double basePitch; // SAPI 5: Hz, 0 = the voice's own
        final String emotion;   // OneCore: "", "happy", "sad", "angry"
        final boolean female;
        final Locale locale;

        Def(String slug, String display, int engine, String name, String effect, double basePitch, String emotion,
            boolean female, String language) {
            this.slug = slug;
            this.display = display;
            this.engine = engine;
            this.name = name;
            this.effect = effect;
            this.basePitch = basePitch;
            this.emotion = emotion;
            this.female = female;
            this.locale = Locale.forLanguageTag(language);
        }

        /** Handles are pooled per engine instance: a OneCore voice's emotions share one (as on iOS). */
        String poolKey() {
            return engine == Engine.ONECORE ? "onecore:" + name : slug;
        }
    }

    static final List<Def> ALL;

    private static Def sapi5(String slug, String display, String spd, String effect, double pitch, boolean female) {
        return new Def(slug, display, Engine.SAPI5, spd, effect, pitch, "", female, "en-US");
    }

    private static Def sapi4(String slug, String display, String mode, boolean female) {
        return new Def(slug, display, Engine.SAPI4, mode, "none", 0, "", female, "en-US");
    }

    private static Def truVoice(String slug, String display, String mode, boolean female) {
        return new Def(slug, display, Engine.TRUVOICE, mode + ", American English (TruVoice)", "none", 0, "", female,
                       "en-US");
    }

    static {
        List<Def> v = new ArrayList<>();
        v.add(sapi5("sam", "Sam (SAPI 5)", "Sam", "none", 0, false));
        v.add(sapi5("mike", "Mike (SAPI 5)", "Mike", "none", 0, false));
        v.add(sapi5("mary", "Mary (SAPI 5)", "Mary", "none", 0, true));
        v.add(sapi5("mike_hall", "Mike in Hall (SAPI 5)", "Mike", "hall", 0, false));
        v.add(sapi5("mike_stadium", "Mike in Stadium (SAPI 5)", "Mike", "stadium", 0, false));
        v.add(sapi5("mike_space", "Mike in Space (SAPI 5)", "Mike", "space", 0, false));
        v.add(sapi5("mary_hall", "Mary in Hall (SAPI 5)", "Mary", "hall", 0, true));
        v.add(sapi5("mary_stadium", "Mary in Stadium (SAPI 5)", "Mary", "stadium", 0, true));
        v.add(sapi5("mary_space", "Mary in Space (SAPI 5)", "Mary", "space", 0, true));
        v.add(sapi5("robosoft1", "RoboSoft One (SAPI 5)", "Sam", "robosoft1", 75, false));
        v.add(sapi5("robosoft2", "RoboSoft Two (SAPI 5)", "Sam", "robosoft2", 120, false));
        v.add(sapi5("robosoft3", "RoboSoft Three (SAPI 5)", "Mike", "robosoft3", 0, false));
        v.add(sapi5("robosoft4", "RoboSoft Four (SAPI 5)", "Mary", "robosoft4", 0, true));
        v.add(sapi5("robosoft6", "RoboSoft Six (SAPI 5)", "Sam", "robosoft6", 100, false));
        v.add(sapi5("male_whisper", "Male Whisper (SAPI 5)", "Mike", "whisper", 0, false));
        v.add(sapi5("female_whisper", "Female Whisper (SAPI 5)", "Mary", "whisper", 0, true));

        v.add(new Def("anna", "Anna", Engine.ANNA, "", "none", 0, "", true, "en-US"));

        Object[][] onecore = {
            {"David", false, "en-US"}, {"Zira", true, "en-US"}, {"Mark", false, "en-US"},
            {"Hazel", true, "en-GB"}, {"George", false, "en-GB"}, {"Susan", true, "en-GB"},
            {"Eva", true, "en-US"}, {"Sarah", true, "en-GB"},
            {"Catherine", true, "en-AU"}, {"James", false, "en-AU"},
            {"Linda", true, "en-US"}, {"Richard", false, "en-US"},
            {"Matilda", true, "en-AU"},
        };
        List<String> withEmotions = List.of("David", "Zira", "Mark", "Eva");
        for (Object[] o : onecore) {
            String name = (String) o[0];
            List<String> emotions = withEmotions.contains(name) ? List.of("", "happy", "sad", "angry") : List.of("");
            for (String e : emotions) {
                String slug = "onecore_" + name.toLowerCase(Locale.ROOT) + (e.isEmpty() ? "" : "_" + e);
                String display = name + (e.isEmpty() ? "" : " " + Character.toUpperCase(e.charAt(0)) + e.substring(1));
                v.add(new Def(slug, display, Engine.ONECORE, name, "none", 0, e, (Boolean) o[1], (String) o[2]));
            }
        }

        v.add(sapi4("sapi4_sam", "Sam (SAPI 4)", "Sam", false));
        v.add(sapi4("sapi4_mike", "Mike (SAPI 4)", "Mike", false));
        v.add(sapi4("sapi4_mary", "Mary (SAPI 4)", "Mary", true));
        v.add(sapi4("sapi4_mike_telephone", "Mike for Telephone (SAPI 4)", "Mike (for Telephone)", false));
        v.add(sapi4("sapi4_mary_telephone", "Mary for Telephone (SAPI 4)", "Mary (for Telephone)", true));
        v.add(sapi4("sapi4_mike_hall", "Mike in Hall (SAPI 4)", "Mike in Hall", false));
        v.add(sapi4("sapi4_mike_stadium", "Mike in Stadium (SAPI 4)", "Mike in Stadium", false));
        v.add(sapi4("sapi4_mike_space", "Mike in Space (SAPI 4)", "Mike in Space", false));
        v.add(sapi4("sapi4_mary_hall", "Mary in Hall (SAPI 4)", "Mary in Hall", true));
        v.add(sapi4("sapi4_mary_stadium", "Mary in Stadium (SAPI 4)", "Mary in Stadium", true));
        v.add(sapi4("sapi4_mary_space", "Mary in Space (SAPI 4)", "Mary in Space", true));
        v.add(sapi4("sapi4_robosoft1", "RoboSoft One (SAPI 4)", "RoboSoft One", false));
        v.add(sapi4("sapi4_robosoft2", "RoboSoft Two (SAPI 4)", "RoboSoft Two", false));
        v.add(sapi4("sapi4_robosoft3", "RoboSoft Three (SAPI 4)", "RoboSoft Three", false));
        v.add(sapi4("sapi4_robosoft4", "RoboSoft Four (SAPI 4)", "RoboSoft Four", true));
        v.add(sapi4("sapi4_robosoft5", "RoboSoft Five (SAPI 4)", "RoboSoft Five", true));
        v.add(sapi4("sapi4_robosoft6", "RoboSoft Six (SAPI 4)", "RoboSoft Six", true));
        v.add(sapi4("sapi4_male_whisper", "Male Whisper (SAPI 4)", "Male Whisper", false));
        v.add(sapi4("sapi4_female_whisper", "Female Whisper (SAPI 4)", "Female Whisper", true));

        v.add(truVoice("truvoice_adult_male_1", "Peter", "Adult Male #1", false));
        v.add(truVoice("truvoice_adult_male_2", "Sidney", "Adult Male #2", false));
        v.add(truVoice("truvoice_adult_male_3", "Eager Eddie", "Adult Male #3", false));
        v.add(truVoice("truvoice_adult_male_4", "Deep Douglas", "Adult Male #4", false));
        v.add(truVoice("truvoice_adult_male_5", "Biff", "Adult Male #5", false));
        v.add(truVoice("truvoice_adult_male_6", "Grandpa Amos", "Adult Male #6", false));
        v.add(truVoice("truvoice_adult_male_7", "Melvin", "Adult Male #7", false));
        v.add(truVoice("truvoice_adult_male_8", "Alex", "Adult Male #8", false));
        v.add(truVoice("truvoice_adult_female_1", "Wanda", "Adult Female #1", true));
        v.add(truVoice("truvoice_adult_female_2", "Julia", "Adult Female #2", true));
        ALL = Collections.unmodifiableList(v);
    }

    static Def bySlug(String slug) {
        if (slug == null) return null;
        for (Def d : ALL) if (d.slug.equals(slug)) return d;
        return null;
    }

    private Voices() {}
}
