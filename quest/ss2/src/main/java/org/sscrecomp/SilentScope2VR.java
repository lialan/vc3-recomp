// Silent Scope 2 VR for Meta Quest. All of the game runs natively (libmain.so,
// src/android/); this only checks that the player's game files are there and
// loads the libraries.
//
// The game's files (ss2.xbe and data2/) are looked for as
// src/android/android_main.c does:
//   /sdcard/Android/data/org.sscrecomp.ss2/files   (the app's own folder)
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

public class SilentScope2VR extends SDLActivity {
    private static final String TAG = "SilentScope2VR";
    private String problem;     /* null: the game is there */

    @Override
    protected void onCreate(Bundle savedInstanceState) {
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
        return new String[] { "SDL2", "openxr_loader", "main" };
    }

    private String checkGame() {
        File own = getExternalFilesDir(null);
        File shared = new File(Environment.getExternalStorageDirectory(), "SilentScope");
        for (File base : new File[] { own, shared })
            if (base != null && new File(base, "ss2.xbe").canRead() && new File(base, "data2").isDirectory()) {
                writeHelp(own, null);
                return null;
            }
        String msg;
        if (!Environment.isExternalStorageManager()) {
            msg = "Allow Silent Scope 2 VR to access all files, copy the game (ss2.xbe and data2) to "
                + shared + ", then start it again.";
            askForFileAccess();
        } else {
            msg = "Game files not found: copy ss2.xbe and the data2 folder to " + shared;
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
        again.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(again);
        android.os.Process.killProcess(android.os.Process.myPid());
    }

    /* Called from the game (main.c's chihiro_launch_image, through
     * quest_host.c): another image of the disc (XLaunchNewImage) is the
     * sibling app named after it -- org.sscrecomp.ss2 for "ss2" -- which
     * Restart starts once this process is gone. */
    public void launchSibling(String image) {
        String own = getPackageName();
        String pkg = own.substring(0, own.lastIndexOf('.') + 1) + image;
        Log.i(TAG, "launching " + pkg);
        Intent next = new Intent(this, Restart.class);
        next.putExtra(Restart.PID, android.os.Process.myPid());
        next.putExtra(Restart.PACKAGE, pkg);
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
            "Silent Scope 2 VR needs your own copy of the Xbox game (Silent Scope Complete): the\n" +
            "ss2.xbe and its data2 folder.\n\n" +
            "Last start: " + msg + "\n\n" +
            "Copy them so that the game ends up at\n\n" +
            "    /sdcard/SilentScope/ss2.xbe\n" +
            "    /sdcard/SilentScope/data2/...\n\n" +
            "(Internal shared storage > SilentScope over USB, or quest/install.sh), and\n" +
            "allow the app to access all files when it asks.\n";
        try (FileOutputStream out = new FileOutputStream(help)) {
            out.write(text.getBytes("UTF-8"));
        } catch (Exception e) {
            Log.w(TAG, "cannot write " + help + ": " + e);
        }
    }
}
