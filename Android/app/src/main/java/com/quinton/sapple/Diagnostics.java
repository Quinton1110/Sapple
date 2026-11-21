package com.quinton.sapple;

import android.content.Context;
import android.content.pm.PackageInfo;
import android.os.Build;
import android.os.Process;

/** A plain-text report to paste into a bug report: the app and Android versions, which code the app is running
 *  (32- or 64-bit), what voice data is on the phone, and the last thing that failed. It is built only when someone
 *  presses Copy diagnostics, and it never holds anything a person asked to be spoken. */
final class Diagnostics {
    private static String lastFailure = "";

    /** Remembers why a voice failed: the voice's slug and the engine's reason, never the text. */
    static synchronized void recordFailure(Voices.Def d, String reason) {
        lastFailure = d.slug + ": " + (reason == null || reason.isEmpty() ? "no reason given" : reason);
    }

    static synchronized String report(Context ctx, VoiceData data) {
        StringBuilder s = new StringBuilder("Sapple for Android diagnostics\n");
        try {
            PackageInfo p = ctx.getPackageManager().getPackageInfo(ctx.getPackageName(), 0);
            s.append("App version: ").append(p.versionName).append('\n');
        } catch (Exception ignored) {
        }
        s.append("Android: ").append(Build.VERSION.RELEASE).append(" (API ").append(Build.VERSION.SDK_INT).append(")\n");
        s.append("Device: ").append(Build.MANUFACTURER).append(' ').append(Build.MODEL).append('\n');
        s.append("Running as: ").append(Process.is64Bit() ? "64-bit" : "32-bit")
         .append(", device supports ").append(String.join(", ", Build.SUPPORTED_ABIS)).append('\n');
        s.append("Speech SDK for the natural voices: ").append(data.hasSdk() ? "present" : "missing").append('\n');
        String[] names = {"SAPI 5", "SAPI 4", "TruVoice", "Anna", "OneCore", "Natural"};
        int[] have = new int[names.length], all = new int[names.length];
        for (Voices.Def d : Voices.ALL) {
            all[d.engine]++;
            if (data.has(d)) have[d.engine]++;
        }
        s.append("Voices on this phone: ").append(data.availableCount()).append(" of ").append(Voices.ALL.size()).append('\n');
        for (int i = 0; i < names.length; i++) {
            s.append("  ").append(names[i]).append(": ").append(have[i]).append(" of ").append(all[i]).append('\n');
        }
        s.append("Last failure: ").append(lastFailure.isEmpty() ? "none since the app started" : lastFailure).append('\n');
        return s.toString();
    }

    private Diagnostics() {}
}
