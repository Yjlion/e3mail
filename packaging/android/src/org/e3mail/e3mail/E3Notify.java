// SPDX-License-Identifier: MPL-2.0
package org.e3mail.e3mail;

import android.app.Activity;
import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.os.Build;

// New-mail notifications, called from AndroidBackend (NotifierBackends.cpp).
// Only while e3mail runs: there is no background service (ADR 0016).
public final class E3Notify {
    static final String EXTRA_TOKEN = "org.e3mail.e3mail.token";
    private static final String CHANNEL = "mail";
    private static final String PERMISSION = "android.permission.POST_NOTIFICATIONS";
    private static int s_nextId = 1;

    private E3Notify() {}

    public static boolean available(Context ctx) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        return nm != null && nm.areNotificationsEnabled();
    }

    // Android 13 and later ask the person once.
    public static void requestPermission(Context ctx) {
        if (Build.VERSION.SDK_INT < 33 || !(ctx instanceof Activity))
            return;
        if (ctx.checkSelfPermission(PERMISSION) == PackageManager.PERMISSION_GRANTED)
            return;
        final Activity activity = (Activity) ctx;
        activity.runOnUiThread(() -> activity.requestPermissions(new String[] { PERMISSION }, 1));
    }

    public static void show(Context ctx, String channelName, String title, String body, long token) {
        NotificationManager nm = ctx.getSystemService(NotificationManager.class);
        if (nm == null)
            return;
        // Created once; renaming it keeps the person's settings for it.
        nm.createNotificationChannel(
                new NotificationChannel(CHANNEL, channelName, NotificationManager.IMPORTANCE_DEFAULT));

        final int id = s_nextId++;
        Intent intent = new Intent(ctx, E3Activity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP | Intent.FLAG_ACTIVITY_CLEAR_TOP);
        intent.putExtra(EXTRA_TOKEN, token);
        PendingIntent open = PendingIntent.getActivity(
                ctx, id, intent, PendingIntent.FLAG_IMMUTABLE | PendingIntent.FLAG_UPDATE_CURRENT);

        int icon = ctx.getResources().getIdentifier("ic_stat_mail", "drawable", ctx.getPackageName());
        Notification n = new Notification.Builder(ctx, CHANNEL)
                .setSmallIcon(icon != 0 ? icon : ctx.getApplicationInfo().icon)
                .setContentTitle(title)
                .setContentText(body)
                .setCategory(Notification.CATEGORY_EMAIL)
                .setContentIntent(open)
                .setAutoCancel(true)
                .build();
        nm.notify(id, n);
    }
}
