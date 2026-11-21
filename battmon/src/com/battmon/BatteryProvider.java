package com.battmon;

import android.content.BroadcastReceiver;
import android.content.ContentProvider;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.database.Cursor;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.BufferedWriter;
import java.io.FileNotFoundException;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.util.concurrent.LinkedBlockingQueue;
import java.util.concurrent.TimeUnit;

public class BatteryProvider extends ContentProvider {
    private static final String TAG = "BattMon";
    private static final String STREAM = "stream";
    private static final String CURRENT = "current";
    private static final String LINE = "line";

    @Override
    public boolean onCreate() {
        return true;
    }

    @Override
    public ParcelFileDescriptor openFile(Uri uri, String mode) throws FileNotFoundException {
        if (!"r".equals(mode)) throw new FileNotFoundException("read-only");
        if (!STREAM.equals(uri.getLastPathSegment())) {
            throw new FileNotFoundException(uri.toString());
        }
        try {
            ParcelFileDescriptor[] pipe = ParcelFileDescriptor.createPipe();
            final ParcelFileDescriptor read = pipe[0];
            final ParcelFileDescriptor write = pipe[1];
            Thread thread = new Thread(new Runnable() {
                @Override
                public void run() {
                    stream(write);
                }
            }, "battmon-provider");
            thread.setDaemon(true);
            thread.start();
            return read;
        } catch (IOException e) {
            throw new FileNotFoundException("pipe: " + e);
        }
    }

    @Override
    public Bundle call(String method, String arg, Bundle extras) {
        Bundle result = new Bundle();
        if (CURRENT.equals(method)) {
            result.putString(LINE, currentLine());
        }
        return result;
    }

    private String currentLine() {
        Intent intent = batteryIntent();
        return intent == null ? null : BatteryService.format(intent);
    }

    private Intent batteryIntent() {
        IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) {
            return getContext().registerReceiver(null, filter, Context.RECEIVER_NOT_EXPORTED);
        }
        return getContext().registerReceiver(null, filter);
    }

    private void stream(final ParcelFileDescriptor write) {
        final LinkedBlockingQueue<String> queue = new LinkedBlockingQueue<>();
        final String current = currentLine();
        final String[] last = {current};
        BroadcastReceiver receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String line = BatteryService.format(intent);
                if (line != null && !line.equals(last[0])) {
                    last[0] = line;
                    queue.offer(line);
                }
            }
        };
        IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) {
            getContext().registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            getContext().registerReceiver(receiver, filter);
        }

        PrintWriter out = null;
        try {
            out = new PrintWriter(new BufferedWriter(
                    new OutputStreamWriter(new FileOutputStream(write.getFileDescriptor()))), true);
            if (current != null) out.println(current);
            while (true) {
                String line = queue.poll(30, TimeUnit.SECONDS);
                if (line == null) continue;
                out.println(line);
                if (out.checkError()) break;
            }
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
        } finally {
            try {
                getContext().unregisterReceiver(receiver);
            } catch (IllegalArgumentException ignored) {
            }
            if (out != null) out.close();
            try {
                write.close();
            } catch (IOException ignored) {
            }
        }
    }

    @Override
    public String getType(Uri uri) {
        return "text/plain";
    }

    @Override
    public Cursor query(Uri uri, String[] projection, String selection,
                        String[] selectionArgs, String sortOrder) {
        return null;
    }

    @Override
    public Uri insert(Uri uri, ContentValues values) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int delete(Uri uri, String selection, String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }

    @Override
    public int update(Uri uri, ContentValues values, String selection,
                      String[] selectionArgs) {
        throw new UnsupportedOperationException();
    }
}
