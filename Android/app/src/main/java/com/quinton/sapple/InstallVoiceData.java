package com.quinton.sapple;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;

/** "Install voice data" in Android's speech settings: open the app, where the import is. */
public class InstallVoiceData extends Activity {
    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }
}
