// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;

/** Puts the attempt in the job queue after every boot until it is done. */
public final class BootReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (!Intent.ACTION_BOOT_COMPLETED.equals(intent.getAction())) return;
        // Before the queue, because it is about what the reader can open rather
        // than about what this application will do by itself: the entry is
        // there exactly while WeRead is not installed, and this receiver is the
        // one component of this application that a retired entry leaves running.
        StatusActivity.followWhetherWeReadIsInstalled(context);
        Installer.schedule(context);
    }
}
