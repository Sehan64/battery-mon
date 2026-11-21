package com.battmon;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.IntentFilter;
import android.net.LocalServerSocket;
import android.net.LocalSocket;
import android.os.BatteryManager;
import android.os.Build;
import android.os.IBinder;
import android.util.Log;

import java.io.BufferedWriter;
import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.OutputStreamWriter;
import java.io.PrintWriter;
import java.util.ArrayList;
import java.util.List;
import java.util.Locale;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.Executors;

public class BatteryService extends Service {
    private static final String TAG = "BattMon";
    private static final String CHANNEL = "battmon";
    static final String SOCKET_NAME = "com.battmon.battery";
    static final String SHARED_FILE = "/sdcard/Android/media/com.battmon/battery.log";

    private final List<PrintWriter> clients = new ArrayList<>();
    private final ExecutorService sender = Executors.newSingleThreadExecutor();
    private LocalServerSocket server;
    private BroadcastReceiver receiver;
    private FileOutputStream sharedOut;
    private BufferedWriter sharedWriter;
    private volatile boolean running = true;
    private volatile String latest = "";

    @Override
    public void onCreate() {
        super.onCreate();

        NotificationChannel channel = new NotificationChannel(
                CHANNEL, "Battery stream", NotificationManager.IMPORTANCE_LOW);
        channel.setDescription("Battery changes on " + SOCKET_NAME);
        getSystemService(NotificationManager.class).createNotificationChannel(channel);

        Notification notification = new Notification.Builder(this, CHANNEL)
                .setContentTitle("Battery stream active")
                .setContentText("Abstract socket " + SOCKET_NAME)
                .setSmallIcon(android.R.drawable.ic_dialog_info)
                .setOngoing(true)
                .build();
        startForeground(1, notification);

        receiver = new BroadcastReceiver() {
            @Override
            public void onReceive(Context context, Intent intent) {
                String line = format(intent);
                if (line == null || line.equals(latest)) return;
                latest = line;
                Log.i(TAG, line);
                sender.execute(new Runnable() {
                    @Override
                    public void run() {
                        send(line);
                    }
                });
            }
        };

        IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        if (Build.VERSION.SDK_INT >= 33) {
            registerReceiver(receiver, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            registerReceiver(receiver, filter);
        }

        startServer();
        openSharedFile();
        refreshLatest();
        if (!latest.isEmpty()) appendShared(latest);
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        return START_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public void onDestroy() {
        running = false;
        if (receiver != null) unregisterReceiver(receiver);
        sender.shutdownNow();
        try {
            if (server != null) server.close();
        } catch (IOException ignored) {
        }
        synchronized (clients) {
            for (PrintWriter client : clients) client.close();
            clients.clear();
        }
        closeSharedFile();
        super.onDestroy();
    }

    private void refreshLatest() {
        IntentFilter filter = new IntentFilter(Intent.ACTION_BATTERY_CHANGED);
        Intent intent;
        if (Build.VERSION.SDK_INT >= 33) {
            intent = registerReceiver(null, filter, Context.RECEIVER_NOT_EXPORTED);
        } else {
            intent = registerReceiver(null, filter);
        }
        if (intent == null) return;
        String line = format(intent);
        if (line != null) latest = line;
    }

    private void startServer() {
        Thread thread = new Thread(new Runnable() {
            @Override
            public void run() {
                try {
                    server = new LocalServerSocket(SOCKET_NAME);
                    Log.i(TAG, "listening on abstract socket " + SOCKET_NAME);
                    while (running) {
                        LocalSocket socket = server.accept();
                        PrintWriter writer = new PrintWriter(new BufferedWriter(
                                new OutputStreamWriter(socket.getOutputStream())), true);
                        refreshLatest();
                        synchronized (clients) {
                            if (!latest.isEmpty()) writer.println(latest);
                            clients.add(writer);
                        }
                    }
                } catch (IOException e) {
                    if (running) Log.e(TAG, "socket server: " + e);
                }
            }
        }, "battmon-socket");
        thread.setDaemon(true);
        thread.start();
    }

    private void send(String line) {
        synchronized (clients) {
            for (int i = clients.size() - 1; i >= 0; i--) {
                PrintWriter writer = clients.get(i);
                writer.println(line);
                if (writer.checkError()) {
                    writer.close();
                    clients.remove(i);
                }
            }
        }
        appendShared(line);
    }

    private void openSharedFile() {
        File[] dirs = getExternalMediaDirs();
        if (dirs == null || dirs.length == 0 || dirs[0] == null) return;
        File dir = dirs[0];
        if (!dir.exists() && !dir.mkdirs()) return;
        try {
            sharedOut = new FileOutputStream(new File(dir, "battery.log"), true);
            sharedWriter = new BufferedWriter(new OutputStreamWriter(sharedOut));
        } catch (IOException e) {
            Log.w(TAG, "shared file: " + e);
            closeSharedFile();
        }
    }

    private synchronized void appendShared(String line) {
        if (sharedWriter == null) return;
        try {
            if (sharedOut.getChannel().size() > 256 * 1024) {
                sharedOut.getChannel().truncate(0);
                sharedOut.getChannel().position(0);
            }
            sharedWriter.write(line);
            sharedWriter.write('\n');
            sharedWriter.flush();
        } catch (IOException e) {
            Log.w(TAG, "shared file: " + e);
            closeSharedFile();
        }
    }

    private synchronized void closeSharedFile() {
        try {
            if (sharedWriter != null) sharedWriter.close();
        } catch (IOException ignored) {
        }
        sharedWriter = null;
        sharedOut = null;
    }

    static String format(Intent intent) {
        int level = intent.getIntExtra(BatteryManager.EXTRA_LEVEL, -1);
        int scale = intent.getIntExtra(BatteryManager.EXTRA_SCALE, 100);
        if (level < 0 || scale <= 0) return null;

        int percent = Math.round(level * 100f / scale);
        int status = intent.getIntExtra(BatteryManager.EXTRA_STATUS,
                BatteryManager.BATTERY_STATUS_UNKNOWN);
        int health = intent.getIntExtra(BatteryManager.EXTRA_HEALTH,
                BatteryManager.BATTERY_HEALTH_UNKNOWN);
        int plugged = intent.getIntExtra(BatteryManager.EXTRA_PLUGGED, 0);
        boolean present = intent.getBooleanExtra(BatteryManager.EXTRA_PRESENT, false);
        int voltageMv = intent.getIntExtra(BatteryManager.EXTRA_VOLTAGE, 0);
        int temperatureTenths = intent.getIntExtra(BatteryManager.EXTRA_TEMPERATURE, 0);

        return String.format(Locale.US,
                "battery=%d%% status=%s health=%s charger=%s present=%d voltage=%dmV temp=%.1fC",
                percent, statusName(status), healthName(health), chargerName(plugged),
                present ? 1 : 0, voltageMv, temperatureTenths / 10.0);
    }

    private static String statusName(int status) {
        switch (status) {
            case BatteryManager.BATTERY_STATUS_CHARGING: return "CHARGING";
            case BatteryManager.BATTERY_STATUS_DISCHARGING: return "DISCHARGING";
            case BatteryManager.BATTERY_STATUS_NOT_CHARGING: return "NOT_CHARGING";
            case BatteryManager.BATTERY_STATUS_FULL: return "FULL";
            default: return "UNKNOWN";
        }
    }

    private static String healthName(int health) {
        switch (health) {
            case BatteryManager.BATTERY_HEALTH_GOOD: return "GOOD";
            case BatteryManager.BATTERY_HEALTH_OVERHEAT: return "OVERHEAT";
            case BatteryManager.BATTERY_HEALTH_DEAD: return "DEAD";
            case BatteryManager.BATTERY_HEALTH_OVER_VOLTAGE: return "OVER_VOLTAGE";
            case BatteryManager.BATTERY_HEALTH_UNSPECIFIED_FAILURE: return "UNSPECIFIED_FAILURE";
            case BatteryManager.BATTERY_HEALTH_COLD: return "COLD";
            default: return "UNKNOWN";
        }
    }

    private static String chargerName(int plugged) {
        if ((plugged & BatteryManager.BATTERY_PLUGGED_AC) != 0) return "AC";
        if ((plugged & BatteryManager.BATTERY_PLUGGED_USB) != 0) return "USB";
        if ((plugged & BatteryManager.BATTERY_PLUGGED_WIRELESS) != 0) return "WIRELESS";
        return "NONE";
    }
}
