package com.zettabridge.core;

import android.app.Activity;
import android.content.res.AssetManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.system.ErrnoException;
import android.system.Os;
import android.system.OsConstants;
import android.util.Log;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileDescriptor;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;

/**
 * T6: runs the zbrun guest test suite inside this app process, next to ART.
 *
 * Declare it in its own process, because a guest exit_group with live guest threads ends the
 * process:
 * <pre>
 * &lt;activity android:name="com.zettabridge.core.GuestTestActivity"
 *           android:process=":guest" android:exported="true" /&gt;
 * </pre>
 */
public class GuestTestActivity extends Activity {
    private static final String TAG = "zbridge-t6";

    private TextView output;
    private final Handler ui = new Handler(Looper.getMainLooper());

    private static final class TestCase {
        final String name;
        final int expectedExit;
        final String[] args;
        final boolean needsGuestLibs;

        TestCase(String name, int expectedExit, boolean needsGuestLibs, String... args) {
            this.name = name;
            this.expectedExit = expectedExit;
            this.needsGuestLibs = needsGuestLibs;
            this.args = args;
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        output = new TextView(this);
        output.setTextIsSelectable(true);
        output.setTypeface(android.graphics.Typeface.MONOSPACE);
        ScrollView scroll = new ScrollView(this);
        scroll.addView(output);
        setContentView(scroll);
        new Thread(this::runSuite, "zbridge-t6").start();
    }

    private void append(String line) {
        Log.i(TAG, line);
        ui.post(() -> output.append(line + "\n"));
    }

    private void runSuite() {
        try {
            File root = new File(getFilesDir(), "zb");
            extractAssets(getAssets(), getFilesDir());
            String guest = new File(root, "guest").getAbsolutePath();
            String guestLibs = new File(root, "guest/lib").getAbsolutePath();

            List<TestCase> cases = new ArrayList<>();
            cases.add(new TestCase("hello_static", 7, false, "world"));
            cases.add(new TestCase("hello_dynamic", 3, false, new File(root, "io.tmp").getAbsolutePath()));
            cases.add(new TestCase("threads_dynamic", 0, false));
            cases.add(new TestCase("kuser_dynamic", 0, false));
            cases.add(new TestCase("signals_dynamic", 134, false));
            cases.add(new TestCase("sigio_race_dynamic", 0, false,
                    new File(root, "sigio_race.tmp").getAbsolutePath()));
            cases.add(new TestCase("syscalls_dynamic", 0, false, new File(root, "syscalls_tmp").getAbsolutePath()));
            // Also check that "hello from arm32 guest" appears in logcat under tag zbguest.
            cases.add(new TestCase("log_dynamic", 0, false));
            cases.add(new TestCase("cxx_dynamic", 0, true));
            cases.add(new TestCase("or_dlopen_dynamic", 0, true, new File(root, "or").getAbsolutePath()));

            int failures = 0;
            for (TestCase test : cases) {
                if (!runCase(root, guest, guestLibs, test)) failures++;
            }
            append(failures == 0 ? "all guest tests passed" : failures + " guest test(s) failed");
        } catch (Exception e) {
            append("suite error: " + e);
            Log.e(TAG, "suite error", e);
        }
    }

    private boolean runCase(File root, String guest, String guestLibs, TestCase test) throws IOException, ErrnoException {
        String[] argv = new String[test.args.length + 1];
        argv[0] = guest + "/" + test.name;
        System.arraycopy(test.args, 0, argv, 1, test.args.length);

        List<String> env = new ArrayList<>();
        for (Map.Entry<String, String> e : System.getenv().entrySet()) {
            // Host loader variables describe 64-bit libraries; the 32-bit guest linker must not see them.
            if (!e.getKey().equals("LD_LIBRARY_PATH") && !e.getKey().equals("LD_PRELOAD")) {
                env.add(e.getKey() + "=" + e.getValue());
            }
        }
        if (test.needsGuestLibs) env.add("LD_LIBRARY_PATH=" + guestLibs);

        File stdoutFile = new File(root, test.name + ".stdout");
        File stderrFile = new File(root, test.name + ".stderr");
        int exit = runWithRedirectedOutput(root.getAbsolutePath() + "/sysroot", argv, env.toArray(new String[0]),
                stdoutFile, stderrFile);

        String expected = new String(Files.readAllBytes(new File(root, "expected/" + test.name + ".out").toPath()),
                StandardCharsets.UTF_8);
        String actual = new String(Files.readAllBytes(stdoutFile.toPath()), StandardCharsets.UTF_8);
        boolean ok = exit == test.expectedExit && actual.equals(expected);
        if (ok) {
            append("PASS " + test.name);
            return true;
        }
        StringBuilder why = new StringBuilder("FAIL " + test.name + ": exit " + exit + ", expected " + test.expectedExit);
        // The app data dir is not reachable from a shell, so show what differs right on screen.
        String[] expectedLines = expected.split("\n", -1);
        String[] actualLines = actual.split("\n", -1);
        for (int i = 0; i < Math.max(expectedLines.length, actualLines.length); i++) {
            String want = i < expectedLines.length ? expectedLines[i] : "<none>";
            String got = i < actualLines.length ? actualLines[i] : "<none>";
            if (!want.equals(got)) why.append("\n  stdout line ").append(i + 1).append(": got '").append(got)
                    .append("', expected '").append(want).append("'");
        }
        String stderr = new String(Files.readAllBytes(stderrFile.toPath()), StandardCharsets.UTF_8);
        if (!stderr.isEmpty()) {
            why.append("\n  stderr: ").append(stderr.length() > 1500 ? stderr.substring(0, 1500) + "..." : stderr);
        }
        append(why.toString());
        Log.e(TAG, why.toString());
        return false;
    }

    /** Guest stdout/stderr are the process fds 1 and 2; point them at files for the call. */
    private static synchronized int runWithRedirectedOutput(String sysroot, String[] argv, String[] envp, File stdoutFile,
                                                            File stderrFile) throws ErrnoException, IOException {
        FileDescriptor savedOut = Os.dup(FileDescriptor.out);
        FileDescriptor savedErr = Os.dup(FileDescriptor.err);
        int mode = OsConstants.O_WRONLY | OsConstants.O_CREAT | OsConstants.O_TRUNC;
        FileDescriptor out = Os.open(stdoutFile.getAbsolutePath(), mode, 0600);
        FileDescriptor err = Os.open(stderrFile.getAbsolutePath(), mode, 0600);
        try {
            Os.dup2(out, 1);
            Os.dup2(err, 2);
            return ZBridge.runExecutable(sysroot, argv, envp);
        } finally {
            Os.dup2(savedOut, 1);
            Os.dup2(savedErr, 2);
            Os.close(out);
            Os.close(err);
            Os.close(savedOut);
            Os.close(savedErr);
        }
    }

    /** Copies every file listed in assets/zb-files.txt into filesDir, keeping the layout. */
    private static void extractAssets(AssetManager assets, File filesDir) throws IOException {
        List<String> files = new ArrayList<>();
        try (BufferedReader reader = new BufferedReader(new InputStreamReader(assets.open("zb-files.txt"),
                StandardCharsets.UTF_8))) {
            String line;
            while ((line = reader.readLine()) != null) {
                if (!line.isEmpty()) files.add(line);
            }
        }
        byte[] buffer = new byte[1 << 16];
        for (String name : files) {
            File target = new File(filesDir, name);
            File parent = target.getParentFile();
            if (parent != null && !parent.isDirectory() && !parent.mkdirs()) {
                throw new IOException("cannot create " + parent);
            }
            try (InputStream in = assets.open(name); OutputStream out = new FileOutputStream(target)) {
                int n;
                while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
            }
        }
    }
}
