package com.zettabridge.launcher;

import android.content.ContentProvider;
import android.content.ContentValues;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.content.Context;

import java.io.File;
import java.io.FileNotFoundException;

public final class CameraFileProvider extends ContentProvider {
    public static Uri uriForFile(Context context, File file) {
        File camera = new File(context.getFilesDir(), "camera-captures");
        if (!file.getParentFile().equals(camera)) {
            throw new IllegalArgumentException("capture outside the shared camera directory: " + file);
        }
        return Uri.parse("content://" + context.getPackageName() + ".zb.camera/" + file.getName());
    }

    @Override public boolean onCreate() { return true; }

    private File fileFor(Uri uri) throws FileNotFoundException {
        String name = uri.getLastPathSegment();
        if (name == null || name.isEmpty() || name.contains("/") || name.contains("..")) {
            throw new FileNotFoundException(uri.toString());
        }
        return new File(new File(getContext().getFilesDir(), "camera-captures"), name);
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        File file = fileFor(uri);
        File dir = file.getParentFile();
        if (dir != null && !dir.exists() && !dir.mkdirs()) throw new FileNotFoundException(dir.toString());
        boolean write = mode != null && mode.contains("w");
        int flags = write
                ? ParcelFileDescriptor.MODE_CREATE | ParcelFileDescriptor.MODE_READ_WRITE
                    | ParcelFileDescriptor.MODE_TRUNCATE
                : ParcelFileDescriptor.MODE_READ_ONLY;
        return ParcelFileDescriptor.open(file, flags);
    }

    @Override public Cursor query(Uri uri, String[] projection, String selection,
                                   String[] selectionArgs, String sortOrder) { return null; }
    @Override public String getType(Uri uri) { return "image/jpeg"; }
    @Override public Uri insert(Uri uri, ContentValues values) { throw new UnsupportedOperationException(); }
    @Override public int delete(Uri uri, String selection, String[] selectionArgs) { return 0; }
    @Override public int update(Uri uri, ContentValues values, String selection,
                                String[] selectionArgs) { return 0; }
}
