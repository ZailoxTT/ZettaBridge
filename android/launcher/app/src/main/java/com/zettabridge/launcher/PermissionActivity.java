package com.zettabridge.launcher;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Bundle;
import android.util.TypedValue;
import android.view.ViewGroup;
import android.widget.CheckBox;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.IOException;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/** Main-process editor for the runtime permissions of one imported plugin. */
public final class PermissionActivity extends Activity {
    static final String EXTRA_PACKAGE = "zb.permission.package";
    private static final int REQUEST_PERMISSIONS = 1001;

    static Intent intent(Context context, String packageName) {
        return new Intent(context, PermissionActivity.class)
                .putExtra(EXTRA_PACKAGE, packageName);
    }

    private PluginRecord record;
    private final Map<String, CheckBox> boxes = new LinkedHashMap<>();

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        String packageName = getIntent().getStringExtra(EXTRA_PACKAGE);
        record = PluginStore.find(this, packageName);
        if (record == null) {
            finish();
            return;
        }

        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);

        TextView intro = new TextView(this);
        intro.setText("Choose which Android permissions " + record.label
                + " may use. Permissions are shared with this launcher and can be changed here or in Android settings.");
        root.addView(intro);

        LinearLayout choices = new LinearLayout(this);
        choices.setOrientation(LinearLayout.VERTICAL);
        choices.setPadding(0, pad, 0, 0);
        List<String> permissions = PermissionPolicy.requestedRuntimePermissions(record);
        if (permissions.isEmpty()) {
            TextView none = new TextView(this);
            none.setText("This app does not request supported runtime permissions.");
            choices.addView(none);
        }
        for (String permission : permissions) {
            CheckBox box = new CheckBox(this);
            box.setText(PermissionPolicy.label(permission));
            box.setChecked(record.grantedPermissions.contains(permission));
            choices.addView(box, new LinearLayout.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
            boxes.put(permission, box);
        }

        ScrollView scroll = new ScrollView(this);
        scroll.addView(choices);
        root.addView(scroll, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f));

        TextView save = new TextView(this);
        save.setTextAppearance(android.R.style.TextAppearance_DeviceDefault_Widget_Button);
        save.setText("Save");
        save.setGravity(android.view.Gravity.CENTER);
        save.setPadding(0, pad, 0, pad);
        save.setClickable(true);
        save.setOnClickListener(v -> save());
        root.addView(save);
        setContentView(root);
    }

    private void save() {
        record.grantedPermissions.clear();
        List<String> request = new ArrayList<>();
        for (Map.Entry<String, CheckBox> entry : boxes.entrySet()) {
            if (entry.getValue().isChecked()) {
                record.grantedPermissions.add(entry.getKey());
                if (!PermissionPolicy.isGranted(this, entry.getKey())) request.add(entry.getKey());
            }
        }
        try {
            record.save();
        } catch (IOException e) {
            new AlertDialog.Builder(this)
                    .setMessage("Cannot save permissions: " + e.getMessage())
                    .setPositiveButton("Close", null)
                    .show();
            return;
        }
        if (!request.isEmpty()) {
            requestPermissions(request.toArray(new String[0]), REQUEST_PERMISSIONS);
        } else {
            finish();
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] results) {
        super.onRequestPermissionsResult(requestCode, permissions, results);
        if (requestCode != REQUEST_PERMISSIONS) return;
        for (int i = 0; i < permissions.length; i++) {
            if (results[i] != PackageManager.PERMISSION_GRANTED) record.grantedPermissions.remove(permissions[i]);
        }
        try {
            record.save();
        } catch (IOException e) {
            Diagnostics.report(this, "cannot save permission result", e, false);
        }
        finish();
    }

    private int dp(int value) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP,
                value, getResources().getDisplayMetrics());
    }
}
