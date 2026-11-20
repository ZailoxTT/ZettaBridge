package com.zettabridge.launcher;

import android.app.Activity;
import android.os.Bundle;
import android.widget.Toast;
import java.util.List;

/**
 * Manifest-declared placeholder activities in :guest, one per launch mode plus fixed
 * orientations. GuestInstrumentation instantiates the plugin activity in their place, so these
 * classes only run when a stub intent arrives without a loadable plugin.
 */
public final class Stubs {
    private Stubs() {}

    public static class Base extends Activity {
        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            Toast.makeText(this, "ZettaBridge: the app could not be loaded (logcat tag zb-launcher)",
                    Toast.LENGTH_LONG).show();
            finish();
        }
    }

    public static class Standard extends Base {}

    public static class Landscape extends Base {}

    public static class Portrait extends Base {}

    public static class SingleTop extends Base {}

    public static class SingleTask extends Base {}
    public static class SingleTask2 extends Base {}
    public static class SingleTask3 extends Base {}
    public static class SingleTask4 extends Base {}

    static final List<Class<? extends Activity>> SINGLE_TASK =
            List.of(SingleTask.class, SingleTask2.class, SingleTask3.class, SingleTask4.class);

    public static class SingleInstance extends Base {}
}
