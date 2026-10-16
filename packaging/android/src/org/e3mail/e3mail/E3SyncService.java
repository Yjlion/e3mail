// SPDX-License-Identifier: MPL-2.0
package org.e3mail.e3mail;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.Service;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.os.Build;
import android.os.IBinder;

// Keeps e3mail's process, and with it the mail connection and device sync,
// running while e3mail is not in front: a foreground service the person
// turns on in Settings (ADR 0018). It does no work of its own; the
// notification it must show says that e3mail is running, and offers Stop.
public final class E3SyncService extends Service {
    private static final String CHANNEL = "sync";
    private static final int ID = 0x5e3;
    private static final String ACTION_STOP = "org.e3mail.e3mail.STOP_SYNC";
    private static final String EXTRA_CHANNEL = "channel";
    private static final String EXTRA_TITLE = "title";
    private static final String EXTRA_TEXT = "text";
    private static final String EXTRA_STOP = "stop";

    // Registered by AndroidSync.cpp: the person pressed Stop.
    static native void stopPressed();

    // Called from AndroidSync.cpp, while e3mail is in front (Android refuses
    // to start a foreground service from the background).
    public static void start(Context ctx, String channel, String title, String text, String stop) {
        Intent i = new Intent(ctx, E3SyncService.class);
        i.putExtra(EXTRA_CHANNEL, channel);
        i.putExtra(EXTRA_TITLE, title);
        i.putExtra(EXTRA_TEXT, text);
        i.putExtra(EXTRA_STOP, stop);
        ctx.startForegroundService(i);
    }

    public static void stop(Context ctx) {
        ctx.stopService(new Intent(ctx, E3SyncService.class));
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (intent != null && ACTION_STOP.equals(intent.getAction())) {
            try {
                stopPressed();
            } catch (UnsatisfiedLinkError e) {
                // e3mail is not running in this process any more.
            }
            stopForeground(STOP_FOREGROUND_REMOVE);
            stopSelf();
            return START_NOT_STICKY;
        }
        NotificationManager nm = getSystemService(NotificationManager.class);
        String channel = intent != null ? intent.getStringExtra(EXTRA_CHANNEL) : null;
        nm.createNotificationChannel(new NotificationChannel(
                CHANNEL, channel != null ? channel : "Sync", NotificationManager.IMPORTANCE_LOW));

        Intent open = new Intent(this, E3Activity.class);
        open.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP);
        PendingIntent openIntent = PendingIntent.getActivity(this, 0, open, PendingIntent.FLAG_IMMUTABLE);
        PendingIntent stopIntent = PendingIntent.getService(
                this, 1, new Intent(this, E3SyncService.class).setAction(ACTION_STOP), PendingIntent.FLAG_IMMUTABLE);
        int icon = getResources().getIdentifier("ic_stat_mail", "drawable", getPackageName());
        Notification n = new Notification.Builder(this, CHANNEL)
                .setSmallIcon(icon != 0 ? icon : getApplicationInfo().icon)
                .setContentTitle(intent != null ? intent.getStringExtra(EXTRA_TITLE) : "e3mail")
                .setContentText(intent != null ? intent.getStringExtra(EXTRA_TEXT) : null)
                .setOngoing(true)
                .setContentIntent(openIntent)
                .addAction(new Notification.Action.Builder(null,
                        intent != null ? intent.getStringExtra(EXTRA_STOP) : "Stop", stopIntent).build())
                .build();
        if (Build.VERSION.SDK_INT >= 34)
            startForeground(ID, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_SPECIAL_USE);
        else
            startForeground(ID, n);
        // If Android ends the process, e3mail is not running to sync: do not
        // come back without it.
        return START_NOT_STICKY;
    }

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }
}
