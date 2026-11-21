package com.quinton.sapple;

import static org.junit.Assert.assertEquals;
import static org.junit.Assume.assumeTrue;

import android.content.Context;

import androidx.test.ext.junit.runners.AndroidJUnit4;
import androidx.test.platform.app.InstrumentationRegistry;

import org.junit.Test;
import org.junit.runner.RunWith;

import java.io.File;
import java.io.FileInputStream;

/** Imports the voice data from a real Sapple download pushed to the device:
 *    adb push Sapple-1.1.1.ipa /data/local/tmp/sapple-import.zip
 *  Skipped when the file is not there. Replaces whatever voice data the app had. */
@RunWith(AndroidJUnit4.class)
public class ImportTest {
    @Test
    public void importsEveryFolderFromADownload() throws Exception {
        File zip = new File("/data/local/tmp/sapple-import.zip");
        assumeTrue("no " + zip, zip.canRead());
        Context ctx = InstrumentationRegistry.getInstrumentation().getTargetContext();
        VoiceData data = new VoiceData(ctx);
        data.clear();
        assertEquals(0, data.availableCount() - (Engine.nativeHasTruVoice() ? 10 : 0));
        int folders;
        try (FileInputStream in = new FileInputStream(zip)) {
            folders = data.importZip(in);
        }
        assertEquals(VoiceData.FOLDERS.length, folders);
        assertEquals(Voices.ALL.size() - (Engine.nativeHasTruVoice() ? 0 : 10), data.availableCount());
        String report = Diagnostics.report(ctx, data);
        org.junit.Assert.assertTrue(report, report.contains("Natural: 7 of 7"));
        org.junit.Assert.assertTrue(report, report.contains("Speech SDK for the natural voices: present"));
    }
}
