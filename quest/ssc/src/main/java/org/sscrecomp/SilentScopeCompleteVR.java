// Silent Scope Complete VR for Meta Quest. All of the game runs natively; this
// only checks that the player's game files are there and loads the libraries.
//
// One app holds the disc: the menu (libmain.so) and each game built into this
// APK (libss1.so..libss3.so, game.conf's BUNDLE). A process runs one of them --
// the menu, or the game its intent names (Restart.IMAGE) -- and the menu
// starting a game, or a game quitting to the menu, restarts this app with the
// other (launchSibling). A game not built in is the sibling app named after
// it, as before.
//
// The game's files (default.xbe and data_title/) are looked for as
// src/android/android_main.c does:
//   /sdcard/Android/data/org.sscrecomp.ssc/files   (the app's own folder)
//   /sdcard/SilentScope                            (where players copy it;
//        needs all-files access, which the app asks for when it finds no game)
// Without the game the app writes HOW-TO-ADD-THE-GAME.txt into its folder
// and closes.
package org.sscrecomp;

import android.content.Intent;
import android.net.Uri;
import android.os.Bundle;
import android.os.Environment;
import android.provider.Settings;
import android.util.Log;
import android.widget.Toast;
import java.io.File;
import java.io.FileOutputStream;
import org.libsdl.app.SDLActivity;

public class SilentScopeCompleteVR extends SDLActivity {
    private static final String TAG = "SilentScopeCompleteVR";
    private String problem;     /* null: the game is there */
    private String image;       /* the game this process runs (lib<image>.so); null: the menu */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        image = bundled(getIntent().getStringExtra(Restart.IMAGE));
        problem = checkGame();
        super.onCreate(savedInstanceState);
        if (problem != null) {
            Toast.makeText(this, problem, Toast.LENGTH_LONG).show();
            finish();
        }
    }

    @Override
    public void loadLibraries() {
        if (problem != null) throw new RuntimeException(problem);   /* SDL then starts nothing */
        super.loadLibraries();
    }

    @Override
    protected String[] getLibraries() {
        return new String[] { "SDL2", "openxr_loader", image != null ? image : "main" };
    }

    /* android_main.c: each image keeps its own settings, saves and pipeline
     * cache in this app (one app, several images). */
    @Override
    protected String[] getArguments() {
        return new String[] { "--bundled" };
    }

    /* The menu's image name: this package's last part ("ssc"). */
    private String menuName() {
        String own = getPackageName();
        return own.substring(own.lastIndexOf('.') + 1);
    }

    /* img if it is a game built into this APK, else null (the menu, or not here). */
    private String bundled(String img) {
        if (img == null || img.equals(menuName()) || !img.matches("[a-z0-9_]+")) return null;
        return new File(getApplicationInfo().nativeLibraryDir, "lib" + img + ".so").exists() ? img : null;
    }

    private String checkGame() {
        File own = getExternalFilesDir(null);
        File shared = new File(Environment.getExternalStorageDirectory(), "SilentScope");
        /* the menu: default.xbe and data_title; a game: ssN.xbe and dataN */
        String xbe = image == null ? "default.xbe" : image + ".xbe";
        String data = image == null ? "data_title" : "data" + image.replaceAll("[^0-9]", "");
        for (File base : new File[] { own, shared })
            if (base != null && new File(base, xbe).canRead() && new File(base, data).isDirectory()) {
                writeHelp(own, null);
                return null;
            }
        String msg;
        if (!Environment.isExternalStorageManager()) {
            msg = "Allow Silent Scope Complete VR to access all files, copy the game (" + xbe + " and " + data + ") to "
                + shared + ", then start it again.";
            askForFileAccess();
        } else {
            msg = "Game files not found: copy " + xbe + " and the " + data + " folder to " + shared;
        }
        writeHelp(own, msg);
        Log.e(TAG, msg);
        return msg;
    }

    /* Called from the game (main.c's relaunch, through quest_host.c):
     * a relaunch of the game (XLaunchNewImage) ends the process. Restart starts
     * the game again once this process is gone. */
    public void restartGame() {
        Log.i(TAG, "restarting the game");
        Intent again = new Intent(this, Restart.class);
        again.putExtra(Restart.PID, android.os.Process.myPid());
        if (image != null) again.putExtra(Restart.IMAGE, image);
        again.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(again);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    /* Called from the game (main.c's chihiro_launch_image, through
     * quest_host.c): another image of the disc (XLaunchNewImage). The menu,
     * or a game built into this APK, is this app again running it; any other
     * is the sibling app named after it -- org.sscrecomp.ss2 for "ss2".
     * Restart starts it once this process is gone. */
    public void launchSibling(String nextImage) {
        Intent next = new Intent(this, Restart.class);
        next.putExtra(Restart.PID, android.os.Process.myPid());
        String here = bundled(nextImage);
        if (here != null || nextImage.equals(menuName())) {
            Log.i(TAG, "starting " + nextImage + " in this app");
            if (here != null) next.putExtra(Restart.IMAGE, here);
        } else {
            String own = getPackageName();
            String pkg = own.substring(0, own.lastIndexOf('.') + 1) + nextImage;
            Log.i(TAG, "launching " + pkg);
            next.putExtra(Restart.PACKAGE, pkg);
        }
        next.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(next);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    private void askForFileAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                                     Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            Log.w(TAG, "no all-files access screen: " + e);
        }
    }

    private void writeHelp(File own, String msg) {
        if (own == null) return;
        File help = new File(own, "HOW-TO-ADD-THE-GAME.txt");
        if (msg == null) { help.delete(); return; }
        String text =
            "Silent Scope Complete VR needs your own copy of the Xbox game (Silent Scope Complete): the\n" +
            "default.xbe and its data_title folder.\n\n" +
            "Last start: " + msg + "\n\n" +
            "Copy them so that the game ends up at\n\n" +
            "    /sdcard/SilentScope/default.xbe\n" +
            "    /sdcard/SilentScope/data_title/...\n\n" +
            "(Internal shared storage > SilentScope over USB, or quest/install.sh), and\n" +
            "allow the app to access all files when it asks.\n";
        try (FileOutputStream out = new FileOutputStream(help)) {
            out.write(text.getBytes("UTF-8"));
        } catch (Exception e) {
            Log.w(TAG, "cannot write " + help + ": " + e);
        }
    }
}
