package com.alexbatalov.fallout2ce;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Typeface;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.TextView;

import java.io.File;
import java.io.IOException;
import java.io.InterruptedIOException;
import java.util.List;
import java.util.concurrent.atomic.AtomicBoolean;

/** Launcher gate: check releases before SDL starts, only for the HD edition. */
public final class HdUpdateActivity extends Activity {
    static final String EXTRA_MANUAL = "check_hd_art";
    private static final String TAG = "HdUpdate";
    private final Handler ui = new Handler(Looper.getMainLooper());
    private final AtomicBoolean finished = new AtomicBoolean();
    private volatile boolean cancel;
    private volatile boolean busy;
    private volatile boolean downloading;
    private TextView message, details, progressText;
    private ImportProgressView progress;
    private Button primary, secondary;

    @Override protected void onCreate(Bundle state) {
        super.onCreate(state);
        File root = getExternalFilesDir(null);
        if (!BuildConfig.HD_EDITION || !GameImport.isInstalled(root)) {
            startGame();
            return;
        }
        final File game;
        try {
            game = GameProfile.gameDirectory(root);
        } catch (IllegalStateException e) {
            Log.w(TAG, "Profile unavailable", e);
            startGame();
            return;
        }
        setContentView(R.layout.activity_import);
        applyFont(findViewById(R.id.import_panel));
        ((TextView) findViewById(R.id.import_title)).setText(R.string.hd_update_title);
        message = findViewById(R.id.import_message);
        details = findViewById(R.id.import_details);
        progressText = findViewById(R.id.import_progress_text);
        progress = findViewById(R.id.import_progress);
        primary = findViewById(R.id.import_primary);
        secondary = findViewById(R.id.import_secondary);
        findViewById(R.id.import_back).setVisibility(View.GONE);
        message.setText(R.string.hd_update_checking);
        details.setVisibility(View.GONE);
        primary.setVisibility(View.GONE);
        secondary.setVisibility(View.GONE);
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(true);
        progressText.setVisibility(View.GONE);

        // DNS and a stalled network can outlive a socket timeout. No connection
        // should hold the launcher on screen longer than this check window.
        ui.postDelayed(() -> {
            if (!busy) {
                Log.i(TAG, "HD manifest check timed out, launching game");
                startGame();
            }
        }, 7000);
        new Thread(() -> {
            try {
                List<HdPacks.Pack> updates = HdPacks.pending(HdPacks.fetchManifest(), game);
                if (updates.isEmpty()) {
                    ui.post(this::startGame);
                } else {
                    ui.post(() -> offer(game, updates));
                }
            } catch (Exception e) {
                Log.w(TAG, "HD manifest unavailable; continuing without update", e);
                ui.post(this::startGame);
            }
        }, "HD manifest check").start();
    }

    private void offer(File game, List<HdPacks.Pack> updates) {
        if (finished.get()) return;
        busy = true; // stop the seven-second startup gate once the choice appears
        double total = 0;
        StringBuilder covers = new StringBuilder();
        for (HdPacks.Pack pack : updates) {
            if (covers.length() > 0) covers.append(", ");
            covers.append(pack.covers.isEmpty() ? pack.name : pack.covers);
            total += pack.size;
        }
        message.setText(getString(R.string.hd_update_offer, covers.toString(), total / 1000000.0));
        details.setText(getString(R.string.hd_update_more, updates.size(), total / 1000000.0));
        details.setVisibility(View.VISIBLE);
        progress.setVisibility(View.GONE);
        primary.setText(R.string.hd_update_download);
        primary.setVisibility(View.VISIBLE);
        primary.setOnClickListener(v -> download(game, updates));
        secondary.setText(R.string.hd_update_later);
        secondary.setVisibility(View.VISIBLE);
        secondary.setOnClickListener(v -> later());
    }

    private void download(File game, List<HdPacks.Pack> updates) {
        primary.setVisibility(View.GONE);
        progress.setVisibility(View.VISIBLE);
        progress.setIndeterminate(false);
        progress.setFraction(0);
        progressText.setVisibility(View.VISIBLE);
        busy = true;
        downloading = true;
        new Thread(() -> {
            try {
                final String[] lastProgress = {""};
                for (HdPacks.Pack pack : updates) {
                    HdPacks.install(game, pack, (p, phase, current, total) -> {
                        if (cancel) throw new InterruptedIOException("HD update deferred");
                        int percent = total > 0 ? (int) (100.0 * current / total) : 0;
                        String key = p.name + ":" + phase + ":" + percent;
                        if (key.equals(lastProgress[0])) return;
                        lastProgress[0] = key;
                        ui.post(() -> {
                            if (finished.get()) return;
                            if (phase.equals("install")) {
                                message.setText(getString(R.string.hd_update_installing, p.covers));
                            } else {
                                message.setText(getString(R.string.hd_update_offer,
                                    p.covers, p.size / 1000000.0));
                            }
                            progress.setFraction(percent / 100f);
                            progressText.setText(getString(R.string.hd_update_progress, p.name, percent));
                        });
                    });
                }
            } catch (IOException | RuntimeException e) {
                Log.w(TAG, "HD update skipped (can retry next launch)", e);
            }
            ui.post(this::startGame);
        }, "HD pack install").start();
    }

    private void later() {
        if (downloading) {
            cancel = true;
            secondary.setEnabled(false);
        } else {
            startGame();
        }
    }

    @Override public void onBackPressed() { later(); }

    private void startGame() {
        if (!finished.compareAndSet(false, true)) return;
        startActivity(new Intent(this, MainActivity.class));
        finish();
    }

    private void applyFont(View view) {
        Typeface font;
        try { font = Typeface.createFromAsset(getAssets(), "ce.dat/fonts/PTM55FT.ttf"); }
        catch (RuntimeException e) { font = Typeface.MONOSPACE; }
        applyFont(view, font);
    }

    private void applyFont(View view, Typeface font) {
        if (view instanceof TextView) ((TextView) view).setTypeface(font);
        else if (view instanceof ViewGroup) {
            ViewGroup group = (ViewGroup) view;
            for (int i = 0; i < group.getChildCount(); i++) applyFont(group.getChildAt(i), font);
        }
    }
}
