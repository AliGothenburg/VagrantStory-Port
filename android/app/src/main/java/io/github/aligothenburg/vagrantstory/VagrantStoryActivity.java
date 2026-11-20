package io.github.aligothenburg.vagrantstory;

import android.content.Intent;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.graphics.drawable.Icon;
import android.os.Build;

import org.libsdl.app.SDLActivity;

// The game runs in libmain.so (tools/sdl_host.cpp); SDL3 is linked into it
public class VagrantStoryActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] { "main" };
    }

    // Called from the game (sdl_host.cpp, android_pin_shortcut): asks the launcher to put a
    // shortcut with this picture (the Rood Inverse taken from the player's disc) on the home
    // screen. The phone asks the player first. False when the launcher cannot pin shortcuts.
    public boolean pinShortcut(final String pngPath) {
        final Bitmap bmp = BitmapFactory.decodeFile(pngPath);
        if (bmp == null) return false;
        final Intent launch = new Intent(this, VagrantStoryActivity.class)
                .setAction(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER);
        final String label = getString(R.string.app_name);

        if (Build.VERSION.SDK_INT >= 26) {
            final ShortcutManager sm = getSystemService(ShortcutManager.class);
            if (sm == null || !sm.isRequestPinShortcutSupported()) return false;
            final ShortcutInfo info = new ShortcutInfo.Builder(this, "rood_inverse")
                    .setShortLabel(label)
                    .setIcon(Icon.createWithAdaptiveBitmap(bmp))
                    .setIntent(launch)
                    .build();
            runOnUiThread(new Runnable() {
                @Override
                public void run() {
                    try {
                        sm.requestPinShortcut(info, null);
                    } catch (Exception e) {
                        android.util.Log.w("VagrantStory", "Kisayol eklenemedi", e);
                    }
                }
            });
            return true;
        }

        // Android 7: the older launcher broadcast (added without asking)
        final Intent add = new Intent("com.android.launcher.action.INSTALL_SHORTCUT")
                .putExtra(Intent.EXTRA_SHORTCUT_INTENT, launch)
                .putExtra(Intent.EXTRA_SHORTCUT_NAME, label)
                .putExtra(Intent.EXTRA_SHORTCUT_ICON, Bitmap.createScaledBitmap(bmp, 192, 192, true))
                .putExtra("duplicate", false);
        sendBroadcast(add);
        return true;
    }
}
