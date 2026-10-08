// SPDX-License-Identifier: MPL-2.0
package org.e3mail.e3mail;

import android.content.Intent;

import org.qtproject.qt.android.bindings.QtActivity;

// Qt's activity, which also hears taps on new-mail notifications: a tap
// brings this activity back (singleTop) with the notification's token.
public class E3Activity extends QtActivity {
    // Registered by AndroidBackend (NotifierBackends.cpp).
    static native void notificationTapped(long token);

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        if (intent == null || !intent.hasExtra(E3Notify.EXTRA_TOKEN))
            return;
        final long token = intent.getLongExtra(E3Notify.EXTRA_TOKEN, 0);
        intent.removeExtra(E3Notify.EXTRA_TOKEN);
        try {
            notificationTapped(token);
        } catch (UnsatisfiedLinkError e) {
            // The app is starting again after Android ended it: it opens on
            // the inbox, as a launch would.
        }
    }
}
