// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.util.Log;

import java.net.URISyntaxException;
import java.util.ArrayList;

/**
 * Hands this entry's cell on the home screen to the application it installed.
 *
 * The entry is on the home screen because the launcher's default layout puts it
 * there, and it takes itself off once WeRead is installed
 * ({@link StatusActivity#retire}). What belongs in that cell afterwards is
 * WeRead: in that cell, wherever the reader has since moved the entry to, under
 * WeRead's own name and icon.
 *
 * Which cell that is, only the launcher knows, and the launcher hands it out.
 * Its workspace table is an exported provider whose read permission is `normal`
 * and whose write permission is `signatureOrSystem`, and an external write makes
 * it reload (LauncherProvider.update calls reloadLauncherIfExternal). This
 * application is privileged and carries the same signing key, so it may write.
 * Nothing in the launcher knows that WeRead exists; this file is the only place
 * where the two facts meet, and it is in the one application that is about WeRead.
 *
 * The row is **updated**, not deleted and added again: an update keeps container,
 * screen, cellX and cellY, which is the whole point. Writing a new row would put
 * the application in the first free cell instead, which is not where the reader
 * left this entry.
 */
final class HomeScreenCell {
    /** The launcher's workspace table, as its own manifest exports it. */
    private static final Uri FAVORITES =
            Uri.parse("content://com.android.launcher3.settings/favorites");
    private static final String ID = "_id";
    private static final String INTENT = "intent";
    private static final String ITEM_TYPE = "itemType";
    private static final String TITLE = "title";
    private static final String ICON = "icon";
    private static final String ICON_PACKAGE = "iconPackage";
    private static final String ICON_RESOURCE = "iconResource";
    private static final String RESTORED = "restored";
    /**
     * LauncherSettings.Favorites.ITEM_TYPE_APPLICATION. An application's row
     * carries no title or icon of its own: the launcher reads both from the
     * package every time it loads, so WeRead appears under its own name even
     * after it updates itself.
     */
    private static final int ITEM_TYPE_APPLICATION = 0;

    private HomeScreenCell() {
    }

    /**
     * Points this entry's row at the launcher activity of {@code installed}, and
     * removes any other row already pointing there -- the launcher adds one of
     * its own when a session says a person asked for the install, and which of
     * the two happens first is not this application's to decide.
     *
     * Does nothing when it cannot: a launcher without that table, an entry whose
     * row the reader has taken off the screen (then the launcher's own row is
     * the only one, and taking it away would leave none), an application with no
     * activity to launch. Every failure leaves the home screen as it was; the
     * entry still retires, because WeRead is installed either way.
     */
    static void handOver(Context context, String installed) {
        Intent launch = context.getPackageManager().getLaunchIntentForPackage(installed);
        if (launch == null || launch.getComponent() == null) {
            Log.w(Installer.TAG, installed + " has no activity to launch; the cell is left alone");
            return;
        }
        ComponentName target = launch.getComponent();
        ComponentName self = new ComponentName(context, StatusActivity.class);
        ContentResolver resolver = context.getContentResolver();
        long ours = -1;
        ArrayList<Long> theirs = new ArrayList<>();
        try (Cursor cursor = resolver.query(FAVORITES, new String[] {ID, INTENT},
                                            null, null, null)) {
            if (cursor == null) {
                Log.w(Installer.TAG, "no workspace to read; the cell is left alone");
                return;
            }
            while (cursor.moveToNext()) {
                ComponentName component = componentOf(cursor.getString(1));
                if (component == null) {
                    continue;
                }
                if (component.equals(self)) {
                    ours = cursor.getLong(0);
                } else if (component.equals(target)) {
                    theirs.add(cursor.getLong(0));
                }
            }
        } catch (RuntimeException e) {
            // A different launcher, or one that refuses the read: not an error
            // of this application's, and not worth failing an install over.
            Log.w(Installer.TAG, "cannot read the workspace; the cell is left alone", e);
            return;
        }
        if (ours < 0) {
            Log.i(Installer.TAG, "this entry is not on the home screen; nothing to hand over");
            return;
        }

        ContentValues values = new ContentValues();
        values.put(INTENT, new Intent(Intent.ACTION_MAIN)
                .addCategory(Intent.CATEGORY_LAUNCHER)
                .setComponent(target)
                .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                          | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED)
                .toUri(0));
        values.put(ITEM_TYPE, ITEM_TYPE_APPLICATION);
        values.putNull(TITLE);
        values.putNull(ICON);
        values.putNull(ICON_PACKAGE);
        values.putNull(ICON_RESOURCE);
        values.put(RESTORED, 0);
        try {
            if (resolver.update(FAVORITES, values, ID + "=?",
                                new String[] {Long.toString(ours)}) != 1) {
                Log.w(Installer.TAG, "the entry's row was gone; the cell is left alone");
                return;
            }
            // Only now: while this entry's row still said "entry", removing the
            // launcher's own row would have left the application with no icon at
            // all if the update had failed.
            for (long id : theirs) {
                resolver.delete(FAVORITES, ID + "=?", new String[] {Long.toString(id)});
            }
            Log.i(Installer.TAG, "the home screen cell now opens " + target.flattenToShortString());
        } catch (RuntimeException e) {
            Log.w(Installer.TAG, "cannot write the workspace; the cell is left alone", e);
        }
    }

    /** The activity a stored row opens, or null for a row that opens nothing. */
    private static ComponentName componentOf(String uri) {
        if (uri == null) {
            return null;
        }
        try {
            return Intent.parseUri(uri, 0).getComponent();
        } catch (URISyntaxException e) {
            return null;
        }
    }
}
