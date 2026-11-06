// Starts the game again after it has closed -- a relaunch of the game
// (XLaunchNewImage) ends its process, and an app cannot exec itself --
// or, with a package, that app: another image of the disc (the launcher,
// another game) is another app, the sibling package named after it. This runs in a process of its
// own (":restart", see the manifest), waits for the game's to end, starts it
// and closes, without showing anything.
package org.sscrecomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import android.widget.Toast;
import java.io.File;

public class Restart extends Activity {
    static final String PID = "pid";
    static final String PACKAGE = "package";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(PID, 0);
        for (int i = 0; i < 50 && pid > 0 && new File("/proc/" + pid).exists(); i++) {
            try { Thread.sleep(100); } catch (InterruptedException e) { break; }
        }
        String pkg = getIntent().getStringExtra(PACKAGE);
        Intent game = pkg != null ? getPackageManager().getLaunchIntentForPackage(pkg) : null;
        if (pkg != null && game == null)
            Toast.makeText(this, pkg + " is not installed", Toast.LENGTH_LONG).show();
        if (game == null)
            game = new Intent(this, SilentScope3VR.class);
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(game);
        finish();
        Process.killProcess(Process.myPid());
    }
}
