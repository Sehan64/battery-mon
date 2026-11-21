package com.battmon;

import android.app.Activity;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.PowerManager;
import android.provider.Settings;
import android.view.ViewGroup;
import android.widget.LinearLayout;
import android.widget.TextView;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);

        float density = getResources().getDisplayMetrics().density;
        int pad = (int) (20 * density);

        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        root.setBackgroundColor(Color.WHITE);

        TextView text = new TextView(this);
        text.setTextSize(16);
        text.setTextColor(Color.BLACK);
        text.setText("Battery stream running.\n\n"
                + "Abstract socket: " + BatteryService.SOCKET_NAME + "\n"
                + "Provider: content://com.battmon.battery/stream\n"
                + "Shared file: " + BatteryService.SHARED_FILE + "\n\n"
                + "Connect from Termux:\n"
                + "  battery_mon --method app_broadcastreceiver_method\n\n"
                + "The socket is used when permitted; otherwise the shared file.");
        root.addView(text, new LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));

        setContentView(root);

        if (Build.VERSION.SDK_INT >= 33) {
            requestPermissions(new String[] {"android.permission.POST_NOTIFICATIONS"}, 1);
        }

        Intent service = new Intent(this, BatteryService.class);
        if (Build.VERSION.SDK_INT >= 26) {
            startForegroundService(service);
        } else {
            startService(service);
        }

        PowerManager power = getSystemService(PowerManager.class);
        if (!power.isIgnoringBatteryOptimizations(getPackageName())) {
            startActivity(new Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,
                    Uri.parse("package:" + getPackageName())));
        }
    }
}
