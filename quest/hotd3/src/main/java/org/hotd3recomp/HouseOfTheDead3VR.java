// House of the Dead III VR for Meta Quest. All of the game runs natively (libmain.so,
// src/android/); this only checks that the player's game files are there and
// loads the libraries.
//
// The game's files (hod3xb.xbe, FS/ and MVI/) are looked for as
// src/android/android_main.c does:
//   /sdcard/Android/data/org.hotd3recomp.vr/files   (the app's own folder)
//   /sdcard/HouseOfTheDead3                            (where players copy it;
//        needs all-files access, which the app asks for when it finds no game)
// Without the game the app writes HOW-TO-ADD-THE-GAME.txt into its folder
// and closes.
package org.hotd3recomp;

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

public class HouseOfTheDead3VR extends SDLActivity {
    private static final String TAG = "HouseOfTheDead3VR";
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
        File shared = new File(Environment.getExternalStorageDirectory(), "HouseOfTheDead3");
        for (File base : new File[] { own, shared })
            if (base != null && new File(base, "hod3xb.xbe").canRead() && new File(base, "FS").isDirectory()) {
                writeHelp(own, null);
                return null;
            }
        String msg;
        if (!Environment.isExternalStorageManager()) {
            msg = "Allow House of the Dead III VR to access all files, copy the game (hod3xb.xbe, FS and MVI) to "
                + shared + ", then start it again.";
            askForFileAccess();
        } else {
            msg = "Game files not found: copy hod3xb.xbe and the FS and MVI folders to " + shared;
        }
        writeHelp(own, msg);
        Log.e(TAG, msg);
        return msg;
    }

    /* Called from the game (main.c's chihiro relaunch, through quest_host.c):
     * leaving the test menu reboots the cabinet. Restart starts the game
     * again once this process is gone. */
    public void restartGame() {
        Log.i(TAG, "restarting the game");
        Intent again = new Intent(this, Restart.class);
        again.putExtra(Restart.PID, android.os.Process.myPid());
        again.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        startActivity(again);
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
            "House of the Dead III VR needs your own copy of the arcade game (Sega Chihiro): the\n" +
            "hod3xb.xbe with its FS and MVI folders.\n\n" +
            "Last start: " + msg + "\n\n" +
            "Copy them so that the game ends up at\n\n" +
            "    /sdcard/HouseOfTheDead3/hod3xb.xbe\n" +
            "    /sdcard/HouseOfTheDead3/FS/...\n" +
            "    /sdcard/HouseOfTheDead3/MVI/...\n\n" +
            "(Internal shared storage > HouseOfTheDead3 over USB, or quest/install.sh), and\n" +
            "allow the app to access all files when it asks.\n";
        try (FileOutputStream out = new FileOutputStream(help)) {
            out.write(text.getBytes("UTF-8"));
        } catch (Exception e) {
            Log.w(TAG, "cannot write " + help + ": " + e);
        }
    }
}
