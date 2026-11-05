// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.app.Activity;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.net.ConnectivityManager;
import android.net.NetworkInfo;
import android.os.Bundle;
import android.os.Handler;
import android.provider.Settings;
import android.util.Log;
import android.view.View;
import android.widget.Button;
import android.widget.TextView;

/**
 * The home screen's entry for WeRead while WeRead is not there yet.
 *
 * Without it the whole arrangement is invisible: the installation is a job
 * that waits for a network, and a device with no Wi-Fi yet says nothing at
 * all, so nobody can tell that this Kindle will fetch WeRead if asked ("微信读
 * 书你说装了，但是用户完全感知不到", 2026-10-19). The entry says where the
 * installation has got to and offers the one thing to do about it, and once
 * WeRead is installed it takes itself off the home screen and leaves the place
 * to WeRead's own icon.
 */
public final class StatusActivity extends Activity {
    /** While a download runs: often enough to be worth looking at, rarely
     *  enough that the panel is not repainting under the reader's eyes. */
    private static final long REFRESH_MS = 10000;

    private final Handler handler = new Handler();
    /** The instance on the screen, if any: see {@link #openWeReadIfInFront}. */
    private static volatile StatusActivity inFront;
    private Installer installer;
    private TextView state, detail;
    private Button action;
    private final Runnable refresh = new Runnable() {
        @Override
        public void run() {
            show();
        }
    };

    @Override
    protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        setContentView(R.layout.status);
        installer = new Installer(this);
        ((TextView) findViewById(R.id.title)).setText(R.string.entry_title);
        state = findViewById(R.id.state);
        detail = findViewById(R.id.detail);
        action = findViewById(R.id.action);
    }

    @Override
    protected void onResume() {
        super.onResume();
        show();
    }

    @Override
    protected void onPause() {
        super.onPause();
        if (inFront == this) inFront = null;
        handler.removeCallbacks(refresh);
    }

    /** Window focus, not resume: a notification shade pulled over this screen
     *  takes the focus without pausing it, and then the reader is not looking
     *  at this screen (a Codex review, 2026-11-04). */
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) inFront = this;
        else if (inFront == this) inFront = null;
    }

    /**
     * The reader who waited on this screen through the download and the install
     * gets WeRead, not the home screen.
     *
     * {@link Installer#installed(int, String)} runs {@link #retire} the moment
     * the session reports success, and disabling this component makes the
     * activity manager finish any instance of it on the screen -- so a reader
     * watching "installing, a minute or two" was dropped on the home screen
     * two seconds after the install, and the "installed / open" state that
     * show() prepares for exactly this moment was never seen (the first
     * clean install of the release build, 2026-11-04: "安装好了不会自动进
     * 入"). Called before retire(), from the same process the result arrives
     * in, so the open happens while this component can still start things.
     */
    static void openWeReadIfInFront(Context context) {
        StatusActivity screen = inFront;
        inFront = null;   // consumed: one open, whoever asks next
        if (screen == null || screen.isFinishing()) return;
        Intent open = context.getPackageManager().getLaunchIntentForPackage(Installer.PACKAGE);
        if (open == null) return;
        try {
            screen.startActivity(open);
        } catch (RuntimeException e) {
            // Gone between the lookup and the start. Not this method's problem
            // to fix, and not a reason to skip the retire() and the notice that
            // follow in the caller.
            Log.w(Installer.TAG, "could not open WeRead after installing it", e);
            return;
        }
        screen.finish();
        // The caller posted "WeRead installed" just before calling this. A
        // reader who is being taken into WeRead this moment has been told; for
        // them the notice would only sit in the shade until swiped (seen on
        // 2026-11-04). Taken down here, so the one install path that is watched
        // leaves no notice behind and the unwatched one (installed in the
        // background) keeps it.
        Installer.quiet(context);
    }

    /** What is true now, in one of the five states this can be in. */
    private void show() {
        handler.removeCallbacks(refresh);
        if (installer.installed()) {
            retire(this);
            set(R.string.entry_installed, 0, R.string.entry_open, new View.OnClickListener() {
                @Override
                public void onClick(View v) {
                    // The same one-shot as openWeReadIfInFront(): a success
                    // broadcast queued behind this tap must not open it again.
                    inFront = null;
                    Intent open = getPackageManager().getLaunchIntentForPackage(Installer.PACKAGE);
                    if (open != null) startActivity(open);
                    finish();
                }
            });
            return;
        }
        if (installer.refused()) {
            set(R.string.entry_refused, R.string.entry_refused_detail, 0, null);
            return;
        }
        int percent = installer.downloadedPercent();
        if (installer.sessionActive()) {
            set(R.string.entry_installing, R.string.installing_detail, 0, null);
            handler.postDelayed(refresh, REFRESH_MS);
            return;
        }
        if (!connected()) {
            set(R.string.entry_no_network, R.string.entry_no_network_detail,
                    R.string.entry_open_wifi, new View.OnClickListener() {
                        @Override
                        public void onClick(View v) {
                            startActivity(new Intent(Settings.ACTION_WIFI_SETTINGS));
                        }
                    });
            return;
        }
        if (percent >= 0) {
            // A percentage was read off the file's length, which says how much
            // has arrived and nothing about whether anything still is. An
            // attempt that failed is retried with a back-off of ten minutes and
            // more, and this screen used to show the same frozen number for all
            // of it with the button hidden, so the one reader who wanted to do
            // something had nothing to touch (a Codex review, 2026-10-30).
            // Whether it is moving is the file's own modification time: the
            // download writes in 64 KiB pieces, so a file untouched for half a
            // minute is not being written.
            //
            // Both ends of the window, because both clocks here are the wall
            // clock and it moves: a Kindle that picks up the time and sets it
            // back leaves a file stamped in the future, and "newer than thirty
            // seconds ago" stays true for as long as the clock takes to catch
            // up -- the button hidden for a day on a download that stopped (a
            // Codex review, 2026-10-30). A stamp in the future is a clock that
            // changed, not a write, so it counts as not moving.
            long since = System.currentTimeMillis() - installer.apk().lastModified();
            boolean moving = since >= 0 && since < 30_000;
            state.setText(getString(R.string.entry_downloading_percent, percent));
            detail.setText(moving ? R.string.entry_downloading_detail : R.string.entry_downloading_stalled_detail);
            detail.setVisibility(View.VISIBLE);
            action.setVisibility(moving ? View.GONE : View.VISIBLE);
            if (!moving) {
                action.setText(R.string.entry_download_now);
                action.setOnClickListener(new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        Installer.scheduleNow(StatusActivity.this);
                        handler.postDelayed(refresh, REFRESH_MS);
                    }
                });
            }
            handler.postDelayed(refresh, REFRESH_MS);
            return;
        }
        set(R.string.entry_ready, R.string.entry_ready_detail, R.string.entry_download_now,
                new View.OnClickListener() {
                    @Override
                    public void onClick(View v) {
                        Installer.scheduleNow(StatusActivity.this);
                        show();
                    }
                });
    }

    private void set(int stateText, int detailText, int buttonText, View.OnClickListener onClick) {
        state.setText(stateText);
        detail.setText(detailText == 0 ? "" : getString(detailText));
        detail.setVisibility(detailText == 0 ? View.GONE : View.VISIBLE);
        action.setVisibility(buttonText == 0 ? View.GONE : View.VISIBLE);
        if (buttonText != 0) {
            action.setText(buttonText);
            action.setOnClickListener(onClick);
        }
    }

    private boolean connected() {
        ConnectivityManager cm = getSystemService(ConnectivityManager.class);
        NetworkInfo network = cm == null ? null : cm.getActiveNetworkInfo();
        return network != null && network.isConnected();
    }

    /**
     * Hands this entry's cell to WeRead and then takes the entry off the screen.
     * In that order, and in one place, because the two are one act: the cell was
     * this entry's only because WeRead was not there yet.
     * {@link HomeScreenCell#handOver} updates the row the launcher keeps for
     * this entry, so WeRead appears in that same cell, wherever the reader has
     * moved it to by then, under WeRead's own name and icon.
     *
     * The cell is handed over here and is the reader's from then on, like
     * every other cell -- until WeRead is removed, when the launcher deletes
     * the row and {@link HomeScreenCell#takeBack} puts the entry's back.
     */
    static void retire(Context context) {
        // Disabling the component makes the activity manager drop any instance
        // of it without a pause callback, so the reference is let go of here
        // rather than left pointing at a screen that is gone.
        inFront = null;
        ComponentName entry = new ComponentName(context, StatusActivity.class);
        PackageManager packages = context.getPackageManager();
        if (packages.getComponentEnabledSetting(entry)
                == PackageManager.COMPONENT_ENABLED_STATE_DISABLED) return;
        HomeScreenCell.handOver(context, Installer.PACKAGE);
        packages.setComponentEnabledSetting(entry,
                PackageManager.COMPONENT_ENABLED_STATE_DISABLED, PackageManager.DONT_KILL_APP);
    }

    /**
     * Whether this entry can be started is the plain fact "WeRead is not
     * installed", and not a latch that was shut once.
     *
     * It used to be the latch, and the reason written here was that an
     * application the reader removes is not reinstalled, so the entry would have
     * nothing left to offer. The first half is right and stays right -- nothing
     * below queues a download. The second half was wrong: what this screen
     * offers a reader whose WeRead is gone is the same button it offers one who
     * never had it (entry_ready, "download now"), and that button is a tap, not
     * an automatic act. With the latch, uninstalling WeRead left this image with
     * no one-tap way to get it back at all, on a device whose whole point is
     * that it needs no computer.
     *
     * Called after every boot (BootReceiver). While WeRead is installed this
     * does nothing, so the entry stays gone, which is what it is for.
     */
    static void followWhetherWeReadIsInstalled(Context context) {
        if (new Installer(context).installed()) {
            return;
        }
        ComponentName entry = new ComponentName(context, StatusActivity.class);
        PackageManager packages = context.getPackageManager();
        if (packages.getComponentEnabledSetting(entry)
                == PackageManager.COMPONENT_ENABLED_STATE_DISABLED) {
            Log.i(Installer.TAG, "WeRead is not installed any more; the entry can be opened again");
            // Owed before enabled, so that a process that dies between the two
            // still owes it at the next boot.
            HomeScreenCell.owe(context);
            packages.setComponentEnabledSetting(entry,
                    PackageManager.COMPONENT_ENABLED_STATE_DEFAULT, PackageManager.DONT_KILL_APP);
        }
        // Enabled is "in all apps". The cell on the home screen was WeRead's
        // since the hand-over and went with it; it comes back as well
        // (2026-11-04, the owner: "首屏幕不会还原「安装微信读书」的 ICON").
        // Owed only on the way back from retired, and until it succeeds: a
        // reader who took the entry off the home screen themselves, with
        // WeRead never installed, is not given it back at every boot, and a
        // launcher that was not answering at the moment of the uninstall does
        // not cost the tile for good.
        if (HomeScreenCell.owed(context)) {
            HomeScreenCell.takeBack(context);
        }
    }
}
