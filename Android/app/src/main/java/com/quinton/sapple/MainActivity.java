package com.quinton.sapple;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioTrack;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.InputStream;
import java.util.ArrayList;
import java.util.List;

/** The app: bring in the voice data, pick the voice TalkBack uses, hear it, and get to Android's speech settings.
 *  Standard widgets only, so TalkBack reads every control by its label, with no custom drawing. */
public class MainActivity extends Activity {
    private static final int PICK_ARCHIVE = 1;

    private VoiceData data;
    private TextView status;
    private Button chooseVoice, preview;
    private EditText previewText;
    private final Handler main = new Handler(Looper.getMainLooper());
    private volatile boolean previewing;
    private volatile AudioTrack track;

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        data = new VoiceData(this);
        setTitle(R.string.app_name);

        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (16 * getResources().getDisplayMetrics().density);
        col.setPadding(pad, pad, pad, pad);

        status = new TextView(this);
        status.setTextSize(18);
        status.setAccessibilityLiveRegion(View.ACCESSIBILITY_LIVE_REGION_POLITE);
        col.addView(status);

        Button importBtn = button(R.string.import_voices, v -> pickArchive());
        col.addView(importBtn);

        TextView importHelp = new TextView(this);
        importHelp.setText(R.string.import_help);
        col.addView(importHelp);

        chooseVoice = button(0, v -> chooseVoice());
        col.addView(chooseVoice);

        previewText = new EditText(this);
        previewText.setHint(R.string.preview_hint);
        col.addView(previewText);

        preview = button(R.string.preview, v -> togglePreview());
        col.addView(preview);

        col.addView(button(R.string.open_tts_settings, v -> openTtsSettings()));
        col.addView(button(R.string.remove_voices, v -> confirmRemove()));

        ScrollView scroll = new ScrollView(this);
        scroll.addView(col);
        setContentView(scroll);
        refresh();
    }

    @Override
    protected void onPause() {
        super.onPause();
        stopPreview();
    }

    private Button button(int label, View.OnClickListener l) {
        Button b = new Button(this);
        if (label != 0) b.setText(label);
        b.setOnClickListener(l);
        return b;
    }

    private void refresh() {
        int n = data.availableCount();
        if (n == 0) {
            status.setText(R.string.status_none);
        } else {
            status.setText(getResources().getQuantityString(R.plurals.status_count, n, n));
        }
        Voices.Def d = SappleTtsService.chosenVoice(this, data);
        chooseVoice.setText(getString(R.string.voice_now, d != null ? d.display : getString(R.string.voice_none)));
        chooseVoice.setEnabled(n > 0);
        preview.setEnabled(n > 0);
    }

    // ---- import ----

    private void pickArchive() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        i.addCategory(Intent.CATEGORY_OPENABLE);
        i.setType("*/*");
        startActivityForResult(i, PICK_ARCHIVE);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent intent) {
        super.onActivityResult(request, result, intent);
        if (request != PICK_ARCHIVE || result != RESULT_OK || intent == null || intent.getData() == null) return;
        Uri uri = intent.getData();
        status.setText(R.string.importing);
        new Thread(() -> {
            String msg;
            try (InputStream in = getContentResolver().openInputStream(uri)) {
                if (in == null) throw new java.io.IOException("could not open the file");
                int folders = data.importZip(in);
                msg = folders == 0 ? getString(R.string.import_none)
                                   : getString(R.string.import_done, data.availableCount());
            } catch (Exception e) {
                msg = getString(R.string.import_failed, String.valueOf(e.getMessage()));
            }
            final String m = msg;
            main.post(() -> {
                refresh();
                status.setText(m);
            });
        }, "sapple-import").start();
    }

    // ---- voice choice ----

    private void chooseVoice() {
        List<Voices.Def> have = new ArrayList<>();
        for (Voices.Def d : Voices.ALL) if (data.has(d)) have.add(d);
        if (have.isEmpty()) return;
        String[] names = new String[have.size()];
        Voices.Def current = SappleTtsService.chosenVoice(this, data);
        int checked = 0;
        for (int i = 0; i < names.length; i++) {
            names[i] = have.get(i).display;
            if (have.get(i) == current) checked = i;
        }
        new AlertDialog.Builder(this)
                .setTitle(R.string.choose_voice)
                .setSingleChoiceItems(names, checked, (dlg, which) -> {
                    getSharedPreferences(SappleTtsService.PREFS, MODE_PRIVATE).edit()
                            .putString(SappleTtsService.KEY_VOICE, have.get(which).slug).apply();
                    dlg.dismiss();
                    refresh();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }

    // ---- preview ----

    private void togglePreview() {
        if (previewing) {
            stopPreview();
            return;
        }
        Voices.Def d = SappleTtsService.chosenVoice(this, data);
        if (d == null) return;
        String t = previewText.getText().toString().trim();
        final String text = t.isEmpty() ? getString(R.string.preview_default, d.display) : t;
        previewing = true;
        preview.setText(R.string.stop);
        new Thread(() -> {
            int min = AudioTrack.getMinBufferSize(Engine.SAMPLE_RATE, AudioFormat.CHANNEL_OUT_MONO,
                    AudioFormat.ENCODING_PCM_16BIT);
            AudioTrack at = new AudioTrack.Builder()
                    .setAudioAttributes(new AudioAttributes.Builder()
                            .setUsage(AudioAttributes.USAGE_MEDIA)
                            .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                    .setAudioFormat(new AudioFormat.Builder().setSampleRate(Engine.SAMPLE_RATE)
                            .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                            .setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build())
                    .setBufferSizeInBytes(Math.max(min, 8192))
                    .setTransferMode(AudioTrack.MODE_STREAM)
                    .build();
            track = at;
            at.play();
            String[] err = new String[1];
            long[] frames = {0};
            int rc = Engine.speak(data, d, text, 0, 0, 4096, (buf, len) -> {
                if (!previewing) return false;
                at.write(buf, 0, len);
                frames[0] += len / 2;
                return previewing;
            }, err);
            if (previewing) {
                // Let the queued audio play out before releasing the track.
                at.stop();
                while (previewing && (at.getPlaybackHeadPosition() & 0xffffffffL) < frames[0]) {
                    try { Thread.sleep(20); } catch (InterruptedException ie) { break; }
                    if (at.getPlayState() == AudioTrack.PLAYSTATE_STOPPED
                            && (at.getPlaybackHeadPosition() & 0xffffffffL) >= frames[0]) break;
                }
            } else {
                at.pause();
                at.flush();
            }
            at.release();
            track = null;
            final int code = rc;
            main.post(() -> {
                previewing = false;
                preview.setText(R.string.preview);
                if (code < 0) status.setText(getString(R.string.preview_failed, err[0] != null ? err[0] : ""));
            });
        }, "sapple-preview").start();
    }

    private void stopPreview() {
        previewing = false;
    }

    // ---- settings ----

    private void openTtsSettings() {
        try {
            startActivity(new Intent("com.android.settings.TTS_SETTINGS"));
        } catch (ActivityNotFoundException e) {
            startActivity(new Intent(Settings.ACTION_SETTINGS));
        }
    }

    private void confirmRemove() {
        new AlertDialog.Builder(this)
                .setMessage(R.string.remove_confirm)
                .setPositiveButton(R.string.remove_voices, (d, w) -> {
                    data.clear();
                    refresh();
                })
                .setNegativeButton(android.R.string.cancel, null)
                .show();
    }
}
