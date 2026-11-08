package com.zettabridge.launcher;

import android.Manifest;
import android.content.Context;
import android.content.pm.PackageManager;
import android.os.Build;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Shared policy and labels for runtime permissions exposed in the plugin settings panel. */
final class PermissionPolicy {
    private static final Map<String, String> LABELS = new LinkedHashMap<>();

    static {
        LABELS.put(Manifest.permission.CAMERA, "Camera");
        LABELS.put(Manifest.permission.RECORD_AUDIO, "Microphone");
        LABELS.put(Manifest.permission.READ_EXTERNAL_STORAGE, "Storage");
        LABELS.put(Manifest.permission.WRITE_EXTERNAL_STORAGE, "Storage");
        LABELS.put(Manifest.permission.READ_MEDIA_IMAGES, "Photos and videos");
        LABELS.put(Manifest.permission.READ_MEDIA_VIDEO, "Videos");
        LABELS.put(Manifest.permission.READ_MEDIA_AUDIO, "Music and audio");
        LABELS.put(Manifest.permission.ACCESS_FINE_LOCATION, "Precise location");
        LABELS.put(Manifest.permission.ACCESS_COARSE_LOCATION, "Approximate location");
        LABELS.put(Manifest.permission.READ_CALENDAR, "Read calendar");
        LABELS.put(Manifest.permission.WRITE_CALENDAR, "Change calendar");
        LABELS.put(Manifest.permission.READ_CONTACTS, "Contacts");
        LABELS.put(Manifest.permission.WRITE_CONTACTS, "Change contacts");
        LABELS.put(Manifest.permission.READ_PHONE_STATE, "Phone state");
        LABELS.put(Manifest.permission.CALL_PHONE, "Make phone calls");
        LABELS.put(Manifest.permission.READ_CALL_LOG, "Call history");
        LABELS.put(Manifest.permission.WRITE_CALL_LOG, "Change call history");
        LABELS.put(Manifest.permission.ADD_VOICEMAIL, "Voicemail");
        LABELS.put(Manifest.permission.USE_SIP, "Make SIP calls");
        LABELS.put(Manifest.permission.BODY_SENSORS, "Body sensors");
        LABELS.put(Manifest.permission.POST_NOTIFICATIONS, "Notifications");
        LABELS.put(Manifest.permission.SEND_SMS, "Send SMS");
        LABELS.put(Manifest.permission.READ_SMS, "Read SMS");
        LABELS.put(Manifest.permission.RECEIVE_SMS, "Receive SMS");
        LABELS.put(Manifest.permission.RECEIVE_WAP_PUSH, "Receive messages");
        LABELS.put(Manifest.permission.RECEIVE_MMS, "Receive multimedia messages");
        LABELS.put(Manifest.permission.BLUETOOTH_CONNECT, "Nearby Bluetooth devices");
        LABELS.put(Manifest.permission.BLUETOOTH_SCAN, "Find nearby Bluetooth devices");
        LABELS.put(Manifest.permission.BLUETOOTH_ADVERTISE, "Advertise via Bluetooth");
        LABELS.put(Manifest.permission.NEARBY_WIFI_DEVICES, "Nearby Wi-Fi devices");
    }

    private PermissionPolicy() {}

    static boolean isRuntimePermission(String permission) {
        return LABELS.containsKey(permission);
    }

    static String label(String permission) {
        String label = LABELS.get(permission);
        return label != null ? label : permission;
    }

    static boolean isAvailableOnThisDevice(String permission) {
        if (Build.VERSION.SDK_INT >= 33
                && (Manifest.permission.READ_EXTERNAL_STORAGE.equals(permission)
                || Manifest.permission.WRITE_EXTERNAL_STORAGE.equals(permission))) {
            return false;
        }
        if (Build.VERSION.SDK_INT < 33 && Manifest.permission.POST_NOTIFICATIONS.equals(permission)) {
            return false;
        }
        if (Build.VERSION.SDK_INT < 31
                && (Manifest.permission.BLUETOOTH_CONNECT.equals(permission)
                || Manifest.permission.BLUETOOTH_SCAN.equals(permission)
                || Manifest.permission.BLUETOOTH_ADVERTISE.equals(permission)
                || Manifest.permission.NEARBY_WIFI_DEVICES.equals(permission))) {
            return false;
        }
        return true;
    }

    static boolean isGranted(Context context, String permission) {
        return context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED;
    }

    static List<String> requestedRuntimePermissions(PluginRecord record) {
        List<String> out = new ArrayList<>();
        for (String permission : record.requestedPermissions) {
            if (isRuntimePermission(permission) && isAvailableOnThisDevice(permission)) out.add(permission);
        }
        Collections.sort(out, (a, b) -> label(a).compareTo(label(b)));
        return out;
    }

    static List<String> missingPermissions(Context context, PluginRecord record) {
        List<String> out = new ArrayList<>();
        for (String permission : record.grantedPermissions) {
            if (isRuntimePermission(permission)
                    && isAvailableOnThisDevice(permission)
                    && !isGranted(context, permission)
                    && record.requestedPermissions.contains(permission)) {
                out.add(permission);
            }
        }
        Collections.sort(out, (a, b) -> label(a).compareTo(label(b)));
        return out;
    }

    static boolean allows(PluginRecord record, String permission) {
        if (!isRuntimePermission(permission)) return true;
        return record.grantedPermissions.contains(permission)
                || record.grantedPermissions.contains(storageEquivalent(permission));
    }

    private static String storageEquivalent(String permission) {
        if (Manifest.permission.READ_EXTERNAL_STORAGE.equals(permission)
                || Manifest.permission.READ_MEDIA_IMAGES.equals(permission)
                || Manifest.permission.READ_MEDIA_VIDEO.equals(permission)
                || Manifest.permission.READ_MEDIA_AUDIO.equals(permission)) {
            return Manifest.permission.READ_EXTERNAL_STORAGE;
        }
        return permission;
    }

    static List<String> defaults() {
        return Arrays.asList(LABELS.keySet().toArray(new String[0]));
    }
}
