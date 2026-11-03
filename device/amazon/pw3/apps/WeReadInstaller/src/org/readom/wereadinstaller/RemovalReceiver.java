// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.content.BroadcastReceiver;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;

/**
 * Brings the entry back the moment WeRead is gone, rather than at the next boot.
 *
 * {@link BootReceiver} checks the same fact, and on a device that is rebooted
 * once a week that is a week in which this image has no one-tap way to install
 * WeRead again -- long enough for the reader to conclude there is none.
 *
 * Only the entry comes back; nothing is downloaded. An application somebody
 * removed is not reinstalled by this application
 * ({@link StatusActivity#followWhetherWeReadIsInstalled}).
 *
 * ACTION_PACKAGE_FULLY_REMOVED and not ACTION_PACKAGE_REMOVED: the first is one
 * of the broadcasts a manifest receiver still gets on API 26 and above, the
 * second is not. It is also the narrower fact -- the package and its data are
 * both gone. A removal that keeps the data (`pm uninstall -k`) sends neither,
 * and BootReceiver is what covers that.
 */
public final class RemovalReceiver extends BroadcastReceiver {
    @Override
    public void onReceive(Context context, Intent intent) {
        if (!Intent.ACTION_PACKAGE_FULLY_REMOVED.equals(intent.getAction())) {
            return;
        }
        Uri data = intent.getData();
        if (data == null || !Installer.PACKAGE.equals(data.getSchemeSpecificPart())) {
            return;
        }
        StatusActivity.followWhetherWeReadIsInstalled(context);
    }
}
