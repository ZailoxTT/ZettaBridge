package com.zettabridge.launcher;

import android.app.Activity;
import android.app.ActivityManager;
import android.app.Application;
import android.app.Instrumentation;
import android.content.ContextWrapper;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.os.Bundle;
import android.util.Log;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.MediaStore;
import android.view.ContextThemeWrapper;
import android.view.Window;

import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.io.IOException;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.util.HashMap;
import java.util.Map;
import android.os.Handler;
import android.os.Looper;

/** Plugin runtime of the :guest process: loaded plugins, intent routing and activity fixups. */
final class GuestRuntime {
    private static final String TAG = "zb-launcher";
    static final String EXTRA_PLUGIN = "com.zettabridge.launcher.PLUGIN";
    static final String EXTRA_ACTIVITY = "com.zettabridge.launcher.ACTIVITY";
    static final String EXTRA_INTENT = "com.zettabridge.launcher.INTENT";

    private static final GuestRuntime INSTANCE = new GuestRuntime();

    private final Map<String, LoadedPlugin> plugins = new HashMap<>();
    private final Map<String, Integer> liveActivities = new HashMap<>();
    private final Map<Uri, File> cameraCaptures = new HashMap<>();
    private Application host;
    private boolean installed;
    private volatile LoadedPlugin current;

    static GuestRuntime get() {
        return INSTANCE;
    }

    boolean isInstalled() {
        return installed;
    }

    void install(Application app) {
        host = app;
        try {
            RuntimeBundle.install(app);
        } catch (IOException e) {
            Diagnostics.report(app, "cannot install the ZettaBridge runtime bundle", e, false);
            return;
        }
        HiddenApi.exemptAll();
        try {
            Class<?> activityThread = Class.forName("android.app.ActivityThread");
            Object thread = Reflect.method(activityThread, "currentActivityThread").invoke(null);
            Field field = Reflect.field(activityThread, "mInstrumentation");
            Instrumentation original = (Instrumentation) field.get(thread);
            if (!(original instanceof GuestInstrumentation)) {
                GuestInstrumentation guest = new GuestInstrumentation(this, original);
                try {
                    // Lets inherited, non-overridden methods see the ActivityThread like the original.
                    Method basicInit = Reflect.method(Instrumentation.class, "basicInit", activityThread);
                    basicInit.invoke(guest, thread);
                } catch (ReflectiveOperationException e) {
                    Log.w(TAG, "Instrumentation.basicInit unavailable: " + e);
                }
                field.set(thread, guest);
            }
            installed = true;
            Log.i(TAG, "guest Instrumentation installed");
            PackageManagerHook.install(app, this);
        } catch (ReflectiveOperationException | RuntimeException e) {
            Log.e(TAG, "cannot install the guest Instrumentation; plugins will not launch", e);
        }
    }

    synchronized LoadedPlugin load(String packageName) throws Exception {
        LoadedPlugin p = plugins.get(packageName);
        if (p != null) {
            current = p;
            return p;
        }
        PluginRecord record = PluginStore.find(host, packageName);
        if (record == null) throw new IllegalStateException(packageName + " is not imported");
        // current is set inside LoadedPlugin.load before the plugin Application runs, so its
        // package manager queries already see the plugin's meta-data.
        p = LoadedPlugin.load(host, record, loaded -> current = loaded);
        plugins.put(packageName, p);
        ActivePlugin.record(host, packageName);
        return p;
    }

    /** The plugin launched most recently; all plugins share the :guest process. */
    LoadedPlugin current() {
        return current;
    }

    /** A plugin component by class name, for the IPackageManager method that asks for it. */
    synchronized android.content.pm.ComponentInfo findComponent(String method, String className) {
        LoadedPlugin cur = current;
        if (cur != null) {
            android.content.pm.ComponentInfo info = componentOf(cur, method, className);
            if (info != null) return info;
        }
        for (LoadedPlugin p : plugins.values()) {
            android.content.pm.ComponentInfo info = componentOf(p, method, className);
            if (info != null) return info;
        }
        return null;
    }

    android.content.pm.ProviderInfo findProvider(String authority) {
        LoadedPlugin plugin = current;
        if (plugin != null) {
            android.content.pm.ProviderInfo info = providerAuthority(plugin, authority);
            if (info != null) return info;
        }
        synchronized (this) {
            for (LoadedPlugin p : plugins.values()) {
                android.content.pm.ProviderInfo info = providerAuthority(p, authority);
                if (info != null) return info;
            }
        }
        return null;
    }

    private static android.content.pm.ProviderInfo providerAuthority(LoadedPlugin p, String authority) {
        for (android.content.pm.ProviderInfo info : p.providerInfos.values()) {
            if (info.authority != null) {
                for (String candidate : info.authority.split(";")) {
                    if (authority.equals(candidate)) return info;
                }
            }
        }
        return null;
    }

    private static android.content.pm.ComponentInfo componentOf(LoadedPlugin p, String method, String className) {
        switch (method) {
            case "getServiceInfo":
                return p.services.get(className);
            case "getActivityInfo":
                return p.activities.get(className);
            case "getReceiverInfo":
                return p.receivers.get(className);
            case "getProviderInfo":
                return p.providerInfos.get(className);
            default:
                return null;
        }
    }

    synchronized boolean isRunning(String packageName) {
        Integer n = liveActivities.get(packageName);
        return n != null && n > 0;
    }

    boolean allowsPluginPermission(String packageName, String permission) {
        LoadedPlugin plugin = current();
        if (plugin == null
                || (!plugin.packageName.equals(packageName) && !host.getPackageName().equals(packageName))) {
            return false;
        }
        return PermissionPolicy.allows(plugin.record, permission);
    }

    boolean currentPluginBelongsTo(String packageName) {
        LoadedPlugin plugin = current();
        return plugin != null
                && (plugin.packageName.equals(packageName) || host.getPackageName().equals(packageName));
    }

    /** Rewrites an intent that names a plugin activity to the matching stub; others pass through. */
    Intent route(Intent intent) {
        if (intent == null || intent.getComponent() == null) return intent;
        if (host.getPackageName().equals(intent.getComponent().getPackageName())
                && intent.getComponent().getClassName().startsWith(Stubs.class.getName())) {
            return intent;  // already a stub intent
        }
        String className = intent.getComponent().getClassName();
        synchronized (this) {
            for (LoadedPlugin p : plugins.values()) {
                ActivityInfo ai = p.activities.get(className);
                if (ai != null) return stubIntent(p, ai, intent);
            }
        }
        return intent;
    }

    /** Give capture intents a real launcher-owned provider URI, even when the plugin used its
     * virtual FileProvider authority. */
    Intent prepareCameraIntent(Intent intent) {
        if (intent == null || !MediaStore.ACTION_IMAGE_CAPTURE.equals(intent.getAction())) return intent;
        Uri original = intent.getParcelableExtra(MediaStore.EXTRA_OUTPUT);
        if (original == null) return intent;
        File dir = new File(host.getFilesDir(), "camera-captures");
        if (!dir.exists() && !dir.mkdirs()) {
            Diagnostics.report(host, "cannot create shared camera directory", new IOException(dir.toString()), false);
            return intent;
        }
        File capture = new File(dir, "capture-" + System.currentTimeMillis() + ".jpg");
        Uri shared = CameraFileProvider.uriForFile(host, capture);
        intent.putExtra(MediaStore.EXTRA_OUTPUT, shared);
        intent.addFlags(Intent.FLAG_GRANT_WRITE_URI_PERMISSION | Intent.FLAG_GRANT_READ_URI_PERMISSION);
        synchronized (cameraCaptures) {
            cameraCaptures.put(original, capture);
        }
        new Handler(Looper.getMainLooper()).postDelayed(this::finishCameraCaptureSoon, 500);
        Log.i(TAG, "camera capture redirected from " + original + " to " + shared);
        return intent;
    }

    void finishCameraCapture() {
        Map.Entry<Uri, File> pending = null;
        synchronized (cameraCaptures) {
            if (cameraCaptures.isEmpty()) return;
            for (Map.Entry<Uri, File> entry : cameraCaptures.entrySet()) {
                if (entry.getValue().isFile()) {
                    pending = entry;
                    break;
                }
            }
        }
        if (pending == null) return;
        Uri original = pending.getKey();
        File capture = pending.getValue();
        LoadedPlugin plugin = current;
        if (plugin == null) return;
        android.content.ContentProvider provider = plugin.providerForAuthority(original.getAuthority());
        if (provider == null) {
            Diagnostics.report(host, "camera output provider is unavailable", new IOException(original.toString()), false);
            return;
        }
        try (ParcelFileDescriptor pfd = provider.openFile(original, "w");
             FileInputStream input = new FileInputStream(capture);
             FileOutputStream output = new FileOutputStream(pfd.getFileDescriptor())) {
            byte[] buffer = new byte[8192];
            int count;
            while ((count = input.read(buffer)) != -1) output.write(buffer, 0, count);
            output.getFD().sync();
        } catch (Throwable t) {
            Diagnostics.report(host, "cannot copy camera image to " + original, t, false);
        } finally {
            capture.delete();
            synchronized (cameraCaptures) {
                cameraCaptures.remove(original);
            }
        }
    }

    private void finishCameraCaptureSoon() {
        finishCameraCapture();
        synchronized (cameraCaptures) {
            if (!cameraCaptures.isEmpty()) {
                new Handler(Looper.getMainLooper()).postDelayed(this::finishCameraCaptureSoon, 500);
            }
        }
    }

    private Intent stubIntent(LoadedPlugin p, ActivityInfo ai, Intent original) {
        Intent stub = new Intent(host, stubFor(ai));
        stub.setFlags(original.getFlags());
        stub.putExtra(EXTRA_PLUGIN, p.packageName);
        stub.putExtra(EXTRA_ACTIVITY, ai.name);
        stub.putExtra(EXTRA_INTENT, new Intent(original));
        return stub;
    }

    private static Class<? extends Activity> stubFor(ActivityInfo ai) {
        switch (ai.launchMode) {
            case ActivityInfo.LAUNCH_SINGLE_TOP:
                return Stubs.SingleTop.class;
            case ActivityInfo.LAUNCH_SINGLE_TASK:
                return Stubs.SingleTask.class;
            case ActivityInfo.LAUNCH_SINGLE_INSTANCE:
                return Stubs.SingleInstance.class;
            default:
                break;
        }
        switch (ai.screenOrientation) {
            case ActivityInfo.SCREEN_ORIENTATION_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE:
            case ActivityInfo.SCREEN_ORIENTATION_USER_LANDSCAPE:
                return Stubs.Landscape.class;
            case ActivityInfo.SCREEN_ORIENTATION_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_REVERSE_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT:
            case ActivityInfo.SCREEN_ORIENTATION_USER_PORTRAIT:
                return Stubs.Portrait.class;
            default:
                return Stubs.Standard.class;
        }
    }

    /** Called from Instrumentation.newActivity: the plugin activity for a stub intent, or null. */
    Activity instantiatePluginActivity(Intent intent) {
        if (intent == null) return null;
        String packageName;
        String activity;
        try {
            intent.setExtrasClassLoader(GuestRuntime.class.getClassLoader());
            packageName = intent.getStringExtra(EXTRA_PLUGIN);
            activity = intent.getStringExtra(EXTRA_ACTIVITY);
        } catch (RuntimeException e) {
            return null;
        }
        if (packageName == null || activity == null) return null;
        try {
            LoadedPlugin p = load(packageName);
            return (Activity) p.classLoader.loadClass(activity).getDeclaredConstructor().newInstance();
        } catch (Throwable t) {
            Diagnostics.report(host, "cannot instantiate " + packageName + "/" + activity, t, true);
            return null;
        }
    }

    private synchronized LoadedPlugin pluginOf(Activity activity) {
        ClassLoader cl = activity.getClass().getClassLoader();
        for (LoadedPlugin p : plugins.values()) {
            if (p.classLoader == cl) return p;
        }
        return null;
    }

    /**
     * Runs after Activity.attach and before onCreate. attach() gave the activity the stub's
     * context, and creating its window already cached launcher resources and theme, so those
     * caches are cleared before the plugin theme is applied.
     */
    void prepareActivity(Activity activity, Bundle icicle) {
        LoadedPlugin p = pluginOf(activity);
        if (p == null) return;
        synchronized (this) {
            liveActivities.merge(p.packageName, 1, Integer::sum);
        }
        ActivityInfo ai = p.activities.get(activity.getClass().getName());

        Reflect.trySet(ContextWrapper.class, activity, "mBase", new PluginContext(activity.getBaseContext(), p));
        Reflect.trySet(ContextThemeWrapper.class, activity, "mResources", null);
        Reflect.trySet(ContextThemeWrapper.class, activity, "mTheme", null);
        Reflect.trySet(ContextThemeWrapper.class, activity, "mInflater", null);
        Reflect.trySet(Window.class, activity.getWindow(), "mWindowStyle", null);
        if (p.application != null) Reflect.trySet(Activity.class, activity, "mApplication", p.application);

        int theme = ai != null ? ai.getThemeResource() : p.appInfo.theme;
        activity.setTheme(theme != 0 ? theme : GuestWindowStyle.defaultTheme(p.appInfo.targetSdkVersion));

        GuestWindowStyle.apply(activity, p.record != null && p.record.isTranslated());
        if (p.record != null && p.record.hideAds) AdHider.attach(activity);

        Intent original = activity.getIntent().getParcelableExtra(EXTRA_INTENT);
        if (original != null) {
            original.setExtrasClassLoader(p.classLoader);
            activity.setIntent(original);
        }
        if (icicle != null) icicle.setClassLoader(p.classLoader);

        CharSequence title = p.label;
        if (ai != null) {
            if (ai.screenOrientation != ActivityInfo.SCREEN_ORIENTATION_UNSPECIFIED) {
                activity.setRequestedOrientation(ai.screenOrientation);
            }
            if (ai.softInputMode != 0) activity.getWindow().setSoftInputMode(ai.softInputMode);
            if (ai.labelRes != 0) {
                try {
                    title = p.resources.getText(ai.labelRes);
                } catch (RuntimeException ignored) {
                    // keep the application label
                }
            } else if (ai.nonLocalizedLabel != null) {
                title = ai.nonLocalizedLabel;
            }
        }
        activity.setTitle(title);
        activity.setTaskDescription(new ActivityManager.TaskDescription(p.label, p.icon()));
    }

    void onActivityDestroyed(Activity activity) {
        LoadedPlugin p = pluginOf(activity);
        if (p == null) return;
        synchronized (this) {
            liveActivities.merge(p.packageName, -1, Integer::sum);
        }
    }
}
