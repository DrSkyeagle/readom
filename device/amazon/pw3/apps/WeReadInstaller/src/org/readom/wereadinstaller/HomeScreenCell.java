// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.content.ComponentName;
import android.content.ContentResolver;
import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.Process;
import android.os.UserManager;
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
    private static final String CONTAINER = "container";
    private static final String SCREEN = "screen";
    private static final String CELL_X = "cellX";
    private static final String CELL_Y = "cellY";
    private static final String SPAN_X = "spanX";
    private static final String SPAN_Y = "spanY";
    private static final String PROFILE_ID = "profileId";
    /** LauncherSettings.Favorites.CONTAINER_DESKTOP. */
    private static final int CONTAINER_DESKTOP = -100;
    /** Where the entry's cell was when it was handed over, kept to put it back. */
    private static final String PREFS = "home-screen-cell";
    private static final String KEPT_SCREEN = "screen";
    private static final String KEPT_X = "cellX";
    private static final String KEPT_Y = "cellY";
    /**
     * Set when the entry is enabled again and cleared once its row is back:
     * the enabled state alone cannot carry "still to be put back", so a
     * launcher that was not answering at the moment of the uninstall would
     * otherwise have cost the tile for good (review, 2026-11-04).
     */
    private static final String OWED = "owed";
    /**
     * Where the default layout puts the entry (aosp-launcher3 patch 0010:
     * screen 0, second row), for a device that never recorded a hand-over.
     * The first screen is four cells wide there; cells are tried in that
     * region and no further, because a row outside the launcher's grid is
     * dropped by its loader without a word.
     */
    private static final int DEFAULT_SCREEN = 0, DEFAULT_X = 0, DEFAULT_Y = 1;
    private static final int TRIED_COLUMNS = 4, TRIED_ROWS = 4;
    /**
     * The first row of the first screen is the search box, which is not a
     * favorites row (aosp-launcher3 patches 0015 and 0016 mark it occupied
     * the same way); a row put there is dropped by the loader as a clash.
     */
    private static final int RESERVED_ROWS = 1;
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
        int[] cell = null;
        try (Cursor cursor = resolver.query(FAVORITES,
                                            new String[] {ID, INTENT, CONTAINER, SCREEN, CELL_X, CELL_Y},
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
                    if (cursor.getInt(2) == CONTAINER_DESKTOP) {
                        cell = new int[] {cursor.getInt(3), cursor.getInt(4), cursor.getInt(5)};
                    }
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
        // Remembered before the row is rewritten, so that takeBack() can put
        // the entry where the reader last had it rather than where the default
        // layout once did.
        if (cell != null) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                    .putInt(KEPT_SCREEN, cell[0]).putInt(KEPT_X, cell[1]).putInt(KEPT_Y, cell[2])
                    .apply();
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

    /**
     * Puts the entry's cell back once WeRead is gone.
     *
     * handOver() turned the entry's row into WeRead's; when WeRead is removed
     * the launcher deletes that row, so the cell is empty and nothing refers
     * to this entry any more. Enabling the component again
     * ({@link StatusActivity#followWhetherWeReadIsInstalled}) puts the entry
     * in "all apps" and nowhere else -- which is what the owner saw on
     * 2026-11-04: the tile did not come back, and the one-tap way to install
     * WeRead again had to be found. So the row is added again: at the cell
     * the hand-over remembered when that cell is free, otherwise at the first
     * free cell of the region the default layout uses.
     *
     * Does nothing when the entry already has a row, when the workspace cannot
     * be read, or when no cell in that region is free; each of those is said
     * in the log, and none is an error of this application's.
     */
    /** The entry has just been enabled again; its row is owed until takeBack() succeeds. */
    static void owe(Context context) {
        context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putBoolean(OWED, true).apply();
    }

    static boolean owed(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getBoolean(OWED, false);
    }

    static void takeBack(Context context) {
        ComponentName self = new ComponentName(context, StatusActivity.class);
        ContentResolver resolver = context.getContentResolver();
        boolean[][] taken = new boolean[TRIED_ROWS][TRIED_COLUMNS];
        for (int r = 0; r < RESERVED_ROWS; r++) {
            for (int c = 0; c < TRIED_COLUMNS; c++) {
                taken[r][c] = true;
            }
        }
        boolean keptFree = true;
        android.content.SharedPreferences kept =
                context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
        int screen = kept.getInt(KEPT_SCREEN, DEFAULT_SCREEN);
        int x = kept.getInt(KEPT_X, DEFAULT_X);
        int y = kept.getInt(KEPT_Y, DEFAULT_Y);
        if (screen == DEFAULT_SCREEN && y < RESERVED_ROWS) {
            keptFree = false;
        }
        try (Cursor cursor = resolver.query(FAVORITES,
                new String[] {INTENT, CONTAINER, SCREEN, CELL_X, CELL_Y, SPAN_X, SPAN_Y},
                null, null, null)) {
            if (cursor == null) {
                Log.w(Installer.TAG, "no workspace to read; the cell is not put back");
                return;
            }
            while (cursor.moveToNext()) {
                if (self.equals(componentOf(cursor.getString(0)))) {
                    Log.i(Installer.TAG, "the entry is on the home screen already");
                    kept.edit().remove(OWED).apply();
                    return;
                }
                if (cursor.getInt(1) != CONTAINER_DESKTOP) {
                    continue;
                }
                int rowScreen = cursor.getInt(2), rowX = cursor.getInt(3), rowY = cursor.getInt(4);
                int spanX = Math.max(1, cursor.getInt(5)), spanY = Math.max(1, cursor.getInt(6));
                if (rowScreen == screen && x >= rowX && x < rowX + spanX
                        && y >= rowY && y < rowY + spanY) {
                    keptFree = false;
                }
                if (rowScreen != DEFAULT_SCREEN) {
                    continue;
                }
                for (int r = rowY; r < rowY + spanY && r < TRIED_ROWS; r++) {
                    for (int c = rowX; c < rowX + spanX && c < TRIED_COLUMNS; c++) {
                        if (r >= 0 && c >= 0) {
                            taken[r][c] = true;
                        }
                    }
                }
            }
        } catch (RuntimeException e) {
            Log.w(Installer.TAG, "cannot read the workspace; the cell is not put back", e);
            return;
        }
        if (!keptFree) {
            screen = -1;
            for (int r = 0; r < TRIED_ROWS && screen < 0; r++) {
                for (int c = 0; c < TRIED_COLUMNS; c++) {
                    if (!taken[r][c]) {
                        screen = DEFAULT_SCREEN;
                        x = c;
                        y = r;
                        break;
                    }
                }
            }
            if (screen < 0) {
                Log.i(Installer.TAG, "no free cell on the first screen; the entry stays in all apps");
                return;
            }
        }

        // No _id: the launcher assigns one to every row added from outside
        // (LauncherProvider.initializeExternalAdd), and makes sure the screen
        // exists in its screen table; what this row has to carry is where it
        // goes and what it opens.
        ContentValues values = new ContentValues();
        try {
            values.put(INTENT, new Intent(Intent.ACTION_MAIN)
                    .addCategory(Intent.CATEGORY_LAUNCHER)
                    .setComponent(self)
                    .setFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                              | Intent.FLAG_ACTIVITY_RESET_TASK_IF_NEEDED)
                    .toUri(0));
            values.put(ITEM_TYPE, ITEM_TYPE_APPLICATION);
            values.put(CONTAINER, CONTAINER_DESKTOP);
            values.put(SCREEN, screen);
            values.put(CELL_X, x);
            values.put(CELL_Y, y);
            values.put(SPAN_X, 1);
            values.put(SPAN_Y, 1);
            values.put(RESTORED, 0);
            values.put(PROFILE_ID, context.getSystemService(UserManager.class)
                    .getSerialNumberForUser(Process.myUserHandle()));
            if (resolver.insert(FAVORITES, values) == null) {
                Log.w(Installer.TAG, "the launcher refused the row; the cell is not put back");
                return;
            }
            kept.edit().remove(OWED).apply();
            Log.i(Installer.TAG, "the entry is back on the home screen at " + screen + "/" + x + "," + y
                    + (keptFree ? " (where it was)" : " (first free cell)"));
        } catch (RuntimeException e) {
            Log.w(Installer.TAG, "cannot write the workspace; the cell is not put back", e);
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
