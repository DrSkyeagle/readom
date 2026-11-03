// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInstaller;

/** The result of the install session. */
public final class ResultReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        int status = intent.getIntExtra(PackageInstaller.EXTRA_STATUS, PackageInstaller.STATUS_FAILURE);
        // With INSTALL_PACKAGES no confirmation is asked; this would mean the
        // permission was not granted (privapp-permissions).
        if (status == PackageInstaller.STATUS_PENDING_USER_ACTION) {
            status = PackageInstaller.STATUS_FAILURE;
        }
        new Installer(context).installed(status, intent.getStringExtra(PackageInstaller.EXTRA_STATUS_MESSAGE));
    }
}
