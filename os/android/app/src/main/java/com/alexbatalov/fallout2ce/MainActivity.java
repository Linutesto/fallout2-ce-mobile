package com.alexbatalov.fallout2ce;

import android.content.Intent;
import android.content.Context;
import android.hardware.display.DisplayManager;
import android.view.Display;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.WindowManager;
import org.libsdl.app.SDLSurface;
import android.content.pm.PackageInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.os.Build;
import android.os.Bundle;
import android.view.DisplayCutout;
import android.view.ViewConfiguration;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;

public class MainActivity extends SDLActivity {
    // Application meta-data (AndroidManifest.xml): sustained performance mode.
    private static final String SUSTAINED_PERFORMANCE_META =
        "io.github.imbaner.fallout2ce.SUSTAINED_PERFORMANCE";

    private boolean noExit = false;
    private boolean highRefresh = true;
    private boolean sustainedPerformance = false;
    private volatile float currentRefreshRate = 60.0f;
    private DisplayManager displayManager;
    private final DisplayManager.DisplayListener refreshListener = new DisplayManager.DisplayListener() {
        @Override public void onDisplayAdded(int id) { }
        @Override public void onDisplayRemoved(int id) { }
        @Override public void onDisplayChanged(int id) { updateCurrentRefreshRate(); }
    };

    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new SDLSurface(context) {
            @Override public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                super.surfaceChanged(holder, format, width, height);
                applyRefreshRate();
            }
        };
    }

    // Called by svga.cc after reading fallout2.cfg and when Preferences applies.
    public void setHighRefresh(boolean enabled) {
        runOnUiThread(() -> {
            highRefresh = enabled;
            applyRefreshRate();
        });
    }

    public float getRefreshRate() { return currentRefreshRate; }

    private void updateCurrentRefreshRate() {
        Display display = getWindowManager().getDefaultDisplay();
        currentRefreshRate = display != null ? display.getRefreshRate() : 60.0f;
    }

    private void applyRefreshRate() {
        Display display = getWindowManager().getDefaultDisplay();
        if (display == null) return;
        Display.Mode current = display.getMode();
        Display.Mode[] modes = display.getSupportedModes();
        float[] rates = new float[modes.length];
        for (int i = 0; i < modes.length; i++) {
            // Never change the resolution to obtain a faster mode.
            if (modes[i].getPhysicalWidth() == current.getPhysicalWidth()
                    && modes[i].getPhysicalHeight() == current.getPhysicalHeight()) {
                rates[i] = modes[i].getRefreshRate();
            }
        }
        float target = RefreshRatePolicy.choose(highRefresh, rates);
        WindowManager.LayoutParams params = getWindow().getAttributes();
        params.preferredRefreshRate = target;
        // The exact mode, asked for on every API level: preferredRefreshRate
        // is only a hint, and Surface.setFrameRate (below) does not raise a
        // panel that is already sitting at its current mode. The current mode
        // when it already runs at the target (no switch, no flash), else the
        // first mode at this resolution and rate; 0 is "let the system pick".
        params.preferredDisplayModeId = 0;
        if (Math.abs(current.getRefreshRate() - target) < 0.5f) {
            params.preferredDisplayModeId = current.getModeId();
        } else {
            for (Display.Mode mode : modes) {
                if (mode.getPhysicalWidth() == current.getPhysicalWidth()
                        && mode.getPhysicalHeight() == current.getPhysicalHeight()
                        && Math.abs(mode.getRefreshRate() - target) < 0.5f) {
                    params.preferredDisplayModeId = mode.getModeId();
                    break;
                }
            }
        }
        getWindow().setAttributes(params);
        if (Build.VERSION.SDK_INT >= 30 && mSurface != null) {
            Surface surface = mSurface.getHolder().getSurface();
            if (surface.isValid()) {
                // Games use DEFAULT compatibility, not fixed-rate video.
                surface.setFrameRate(target, Surface.FRAME_RATE_COMPATIBILITY_DEFAULT);
            }
        }
        updateCurrentRefreshRate();
        android.util.Log.i("Wasteland", "Refresh requested=" + target + " actual=" + currentRefreshRate);
    }

    @Override protected void onResume() {
        super.onResume();
        if (displayManager == null) {
            displayManager = (DisplayManager) getSystemService(Context.DISPLAY_SERVICE);
            if (displayManager != null) displayManager.registerDisplayListener(refreshListener, null);
        }
        applyRefreshRate();
        applySustainedPerformance();
    }

    // Sustained performance mode (Window.setSustainedPerformanceMode): the
    // device holds one clock level instead of bursting and throttling, so
    // frame times stay even over a long session, at the cost of peak clocks.
    // The manifest's meta-data decides (an option, see AndroidManifest.xml);
    // devices without the mode are never asked (PowerManager, below).
    private boolean sustainedPerformanceRequested() {
        try {
            android.os.Bundle meta = getPackageManager()
                .getApplicationInfo(getPackageName(), PackageManager.GET_META_DATA).metaData;
            return meta != null && meta.getBoolean(SUSTAINED_PERFORMANCE_META, false);
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    private void applySustainedPerformance() {
        // Window.setSustainedPerformanceMode is void and quietly ignored where
        // there is no such mode; PowerManager is the way to ask first.
        android.os.PowerManager power = (android.os.PowerManager) getSystemService(Context.POWER_SERVICE);
        boolean supported = power != null && power.isSustainedPerformanceModeSupported();
        getWindow().setSustainedPerformanceMode(sustainedPerformance && supported);
        android.util.Log.i("Wasteland", "Sustained performance requested=" + sustainedPerformance
                + " supported=" + supported);
    }

    @Override public void onWindowFocusChanged(boolean focused) {
        super.onWindowFocusChanged(focused);
        if (focused) applyRefreshRate();
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        sustainedPerformance = sustainedPerformanceRequested();
        final File externalFilesDir = getExternalFilesDir(null);
        final File savesDirectory;
        try {
            savesDirectory = GameProfile.gameDirectory(externalFilesDir);
        } catch (IllegalStateException e) {
            noExit = true;
            super.onCreate(savedInstanceState);
            android.widget.Toast.makeText(this, e.getMessage(), android.widget.Toast.LENGTH_LONG).show();
            finish();
            return;
        }
        // A replacement of the game's files cut short (the app died while
        // moving them in): finished before the game reads them.
        if (externalFilesDir != null && GameFiles.isCommitPending(externalFilesDir)) {
            try {
                GameFiles.finish(externalFilesDir);
            } catch (IOException e) {
                e.printStackTrace();
            }
        }
        // The same for an import of saves.
        if (externalFilesDir != null && GameSaves.isCommitPending(savesDirectory)) {
            try {
                GameSaves.finish(savesDirectory);
            } catch (IOException e) {
                e.printStackTrace();
            }
        }
        installBundledFiles(externalFilesDir);

        super.onCreate(savedInstanceState);

        // Without the game's files (or copied in part): the import screen.
        if (!GameImport.isInstalled(externalFilesDir)) {
            final Intent intent = new Intent(this, ImportActivity.class);
            startActivity(intent);

            noExit = true;
            finish();
        }
    }

    // The engine's own data (ce.dat, a folder in the app's assets, see
    // app/build.gradle) next to the game's, when the app is new or updated.
    private void installBundledFiles(File target) {
        if (target == null) {
            return;
        }

        String stamp;
        try {
            PackageInfo info = getPackageManager().getPackageInfo(getPackageName(), 0);
            stamp = info.versionName + " " + info.lastUpdateTime;
        } catch (PackageManager.NameNotFoundException e) {
            return;
        }

        File ceDat = new File(target, "ce.dat");
        File stampFile = new File(ceDat, ".bundled");
        if (stampFile.isFile() && stamp.equals(readText(stampFile))) {
            return;
        }

        deleteRecursively(ceDat);
        try {
            copyAssets("ce.dat", ceDat);
            try (OutputStream out = new FileOutputStream(stampFile)) {
                out.write(stamp.getBytes(StandardCharsets.UTF_8));
            }
        } catch (IOException e) {
            e.printStackTrace();
        }
    }

    private void copyAssets(String path, File target) throws IOException {
        AssetManager assets = getAssets();
        String[] names = assets.list(path);
        if (names == null || names.length == 0) {
            try (InputStream in = assets.open(path); OutputStream out = new FileOutputStream(target)) {
                byte[] buffer = new byte[65536];
                int read;
                while ((read = in.read(buffer)) > 0) {
                    out.write(buffer, 0, read);
                }
            }
            return;
        }

        if (!target.isDirectory() && !target.mkdirs()) {
            throw new IOException("can't make " + target);
        }
        for (String name : names) {
            copyAssets(path + "/" + name, new File(target, name));
        }
    }

    private static void deleteRecursively(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteRecursively(child);
            }
        }
        //noinspection ResultOfMethodCallIgnored
        file.delete();
    }

    private static String readText(File file) {
        try (InputStream in = new FileInputStream(file)) {
            byte[] data = new byte[(int) file.length()];
            int read = in.read(data);
            return new String(data, 0, Math.max(read, 0), StandardCharsets.UTF_8);
        } catch (IOException e) {
            return "";
        }
    }

    @Override
    protected void onDestroy() {
        if (displayManager != null) displayManager.unregisterDisplayListener(refreshListener);
        super.onDestroy();

        if (!noExit) {
            // Needed to make sure libc calls exit handlers, which releases
            // in-game resources.
            System.exit(0);
        }
    }

    // Called from native code (touch.cc): the system's touch & hold delay
    // (Accessibility), so a long press takes as long as in other apps.
    public static int getLongPressTimeout() {
        return ViewConfiguration.getLongPressTimeout();
    }

    // Called from native code (hud_layout.cc): display cutout safe insets in
    // pixels - left, top, right, bottom.
    public static int[] getSafeInsets() {
        final int[] insets = new int[4];
        if (Build.VERSION.SDK_INT >= 28 && mSingleton != null) {
            final WindowInsets windowInsets = mSingleton.getWindow().getDecorView().getRootWindowInsets();
            if (windowInsets != null) {
                final DisplayCutout cutout = windowInsets.getDisplayCutout();
                if (cutout != null) {
                    insets[0] = cutout.getSafeInsetLeft();
                    insets[1] = cutout.getSafeInsetTop();
                    insets[2] = cutout.getSafeInsetRight();
                    insets[3] = cutout.getSafeInsetBottom();
                }
            }
        }
        return insets;
    }

    // Called from native code (mui_preferences.cc, "Game files"): the app's
    // import screen for [mode] (ImportActivity.MODE), in its own process.
    // The saves' import and export go over the game (paused behind it, it
    // reads the saves again when back); replacing the game ([mode] null)
    // closes it (its process exits, see onDestroy) and the import screen
    // starts it again when done.
    public void openGameFiles(String mode) {
        runOnUiThread(() -> {
            Intent intent = new Intent(this, ImportActivity.class);
            if (mode != null) {
                intent.putExtra(ImportActivity.MODE, mode);
            }
            startActivity(intent);
            if (mode == null) {
                finish();
            }
        });
    }

    // Debug builds: the game's command line from the launch intent, for
    // automated tests on a device:
    // adb shell am start -n <package>/.MainActivity --es args "--dev-autotest=<dir> ..."
    @Override
    protected String[] getArguments() {
        // Not when opened again from recent apps: that intent is the old one.
        final boolean fromHistory = (getIntent().getFlags() & Intent.FLAG_ACTIVITY_LAUNCHED_FROM_HISTORY) != 0;
        final String args = BuildConfig.DEBUG && !fromHistory ? getIntent().getStringExtra("args") : null;
        if (args == null || args.trim().isEmpty()) {
            return new String[0];
        }
        return args.trim().split("\\s+");
    }

    @Override
    protected String[] getLibraries() {
        return new String[]{
            "fallout2-ce",
        };
    }
}
