package com.quinton.sapple;

import android.content.Context;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.Locale;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

/** Where the voice data lives on the phone, and how it gets there.
 *
 *  None of the voice data is in this project (it belongs to Microsoft; see NOTICE), and this app does not ship it.
 *  The person installing it brings their own copy: the app reads the data folders out of a Sapple download they
 *  already have (the iPhone .ipa or the Mac .zip, both zip archives with the same folders inside), or out of any zip
 *  holding those folders. TruVoice is the exception: its tables are compiled into the engine, so whether TruVoice is
 *  there depends on the build (see CMakeLists.txt). */
final class VoiceData {
    static final String[] FOLDERS = {"VoiceData", "SAPI4Voices", "AnnaVoice", "OneCoreVoice"};

    private final File root;

    VoiceData(Context ctx) {
        root = new File(ctx.getFilesDir(), "voices");
    }

    String dirFor(int engine) {
        switch (engine) {
            case Engine.SAPI5: return new File(root, "VoiceData").getPath();
            case Engine.SAPI4: return new File(root, "SAPI4Voices").getPath();
            case Engine.ANNA: return new File(root, "AnnaVoice").getPath();
            case Engine.ONECORE: return new File(root, "OneCoreVoice").getPath();
            default: return root.getPath();
        }
    }

    /** Whether this voice's data is on the phone. */
    boolean has(Voices.Def d) {
        switch (d.engine) {
            case Engine.SAPI5: return new File(dirFor(d.engine), d.name + ".spd").isFile();
            case Engine.SAPI4: return new File(dirFor(d.engine), "msttssyn.dll").isFile();
            case Engine.ANNA: return new File(dirFor(d.engine), "M1033DSK.KEY").isFile();
            case Engine.TRUVOICE: return Engine.nativeHasTruVoice();
            case Engine.ONECORE: {
                // The voice's own files are M<language id><Name>.* (M1033David.APM, M2057Hazel.APM ...).
                String[] names = new File(dirFor(d.engine)).list();
                if (names == null) return false;
                java.util.regex.Pattern want = java.util.regex.Pattern.compile(
                        "M\\d+" + java.util.regex.Pattern.quote(d.name) + "\\..+", java.util.regex.Pattern.CASE_INSENSITIVE);
                for (String n : names) if (want.matcher(n).matches()) return true;
                return false;
            }
            default: return false;
        }
    }

    int availableCount() {
        int n = 0;
        for (Voices.Def d : Voices.ALL) if (has(d)) n++;
        return n;
    }

    /** Reads the data folders out of a zip archive (a Sapple .ipa or Mac .zip). Each folder found replaces the copy
     *  on the phone; folders the archive lacks are left alone. Returns how many folders were imported. */
    int importZip(InputStream in) throws IOException {
        File staging = new File(root.getParentFile(), "voices-import");
        deleteTree(staging);
        if (!staging.mkdirs()) throw new IOException("could not make " + staging);
        boolean[] seen = new boolean[FOLDERS.length];
        byte[] buf = new byte[1 << 16];
        try (ZipInputStream zin = new ZipInputStream(in)) {
            ZipEntry e;
            while ((e = zin.getNextEntry()) != null) {
                if (e.isDirectory()) continue;
                String[] parts = e.getName().split("/");
                int at = -1, which = -1;
                for (int i = 0; i < parts.length - 1 && at < 0; i++) {
                    for (int f = 0; f < FOLDERS.length; f++) {
                        if (parts[i].equals(FOLDERS[f])) { at = i; which = f; break; }
                    }
                }
                if (at < 0 || e.getName().contains("__MACOSX")) continue;
                StringBuilder rel = new StringBuilder();
                boolean bad = false;
                for (int i = at + 1; i < parts.length; i++) {
                    String p = parts[i];
                    if (p.isEmpty() || p.equals(".") || p.equals("..") || p.startsWith("._")) { bad = true; break; }
                    if (rel.length() > 0) rel.append('/');
                    rel.append(p);
                }
                if (bad) continue;
                File out = new File(new File(staging, FOLDERS[which]), rel.toString());
                File parent = out.getParentFile();
                if (parent != null && !parent.isDirectory() && !parent.mkdirs()) throw new IOException("could not make " + parent);
                try (OutputStream os = new FileOutputStream(out)) {
                    int n;
                    while ((n = zin.read(buf)) > 0) os.write(buf, 0, n);
                }
                seen[which] = true;
            }
        }
        int count = 0;
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("could not make " + root);
        Engine.releaseIdle();   // nothing may keep reading the files we are about to replace
        for (int f = 0; f < FOLDERS.length; f++) {
            if (!seen[f]) continue;
            File dst = new File(root, FOLDERS[f]);
            deleteTree(dst);
            if (!new File(staging, FOLDERS[f]).renameTo(dst)) throw new IOException("could not move " + FOLDERS[f]);
            count++;
        }
        deleteTree(staging);
        return count;
    }

    /** Removes every imported voice. */
    void clear() {
        Engine.releaseIdle();
        deleteTree(root);
    }

    private static void deleteTree(File f) {
        File[] kids = f.listFiles();
        if (kids != null) for (File k : kids) deleteTree(k);
        //noinspection ResultOfMethodCallIgnored
        f.delete();
    }
}
