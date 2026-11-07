// Starts the game again after it has closed: leaving the test menu reboots
// the cabinet, and an app cannot exec itself. This runs in a process of its
// own (":restart", see the manifest), waits for the game's to end, starts it
// and closes, without showing anything.
package org.hotd3recomp;

import android.app.Activity;
import android.content.Intent;
import android.os.Bundle;
import android.os.Process;
import java.io.File;

public class Restart extends Activity {
    static final String PID = "pid";

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pid = getIntent().getIntExtra(PID, 0);
        for (int i = 0; i < 50 && pid > 0 && new File("/proc/" + pid).exists(); i++) {
            try { Thread.sleep(100); } catch (InterruptedException e) { break; }
        }
        Intent game = new Intent(this, HouseOfTheDead3VR.class);
        game.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK);
        startActivity(game);
        finish();
        Process.killProcess(Process.myPid());
    }
}
