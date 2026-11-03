// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.PendingIntent;
import android.app.job.JobInfo;
import android.app.job.JobScheduler;
import android.content.ComponentName;
import android.content.Context;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageInfo;
import android.content.pm.PackageInstaller;
import android.content.pm.PackageManager;
import android.content.pm.Signature;
import android.os.SystemClock;
import android.util.Log;

import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.locks.ReentrantLock;

/**
 * Installs WeRead once: downloads it from Tencent's official address, checks
 * that it is Tencent's (package name and signing certificate), and installs it
 * without asking. Nothing happens once WeRead has been installed, by this app
 * or by hand, so a user who uninstalls it is not given it back
 * (docs/design/app-install.md).
 */
final class Installer {
    static final String TAG = "WeReadInstaller";
    static final String PACKAGE = "com.tencent.weread.eink";
    // Tencent's e-ink download link (https://ink.qq.com/ink/guide); it
    // redirects to the current release.
    static final String SOURCE = "https://ink.qq.com/web/download_ink";
    // SHA-256 of the certificate WeRead is signed with (APK signature scheme
    // v2). Android requires every update of an app to keep its certificate.
    static final String CERTIFICATE_SHA256 =
            "e4dd2f049941989ffafa45aef52eda9701372c27bc1708e89c217935140bfa1c";
    static final int JOB_ID = 1;
    static final int MAX_REJECTED = 3;

    private static final String STATE = "state";
    private static final String DONE = "done";
    private static final String REJECTED = "rejected";
    private static final String SESSION = "session";
    // Validators of the partial download, so that a resumed download is the
    // same file (the address always gives the current release). The resolved
    // address is not kept: it carries the client's address.
    private static final String ETAG = "etag";
    private static final String LENGTH = "length";
    private static final String CHANNEL = "install";
    private static final int NOTIFICATION = 1;
    /** Held for the whole of one attempt, so there is never a second one writing the file. */
    private static final ReentrantLock BUSY = new ReentrantLock();

    enum Outcome { FINISHED, RETRY }

    private final Context context;
    private final SharedPreferences state;

    Installer(Context context) {
        this.context = context;
        this.state = context.getSharedPreferences(STATE, Context.MODE_PRIVATE);
    }

    /** Schedules the work for the next time a network is connected. */
    static void schedule(Context context) {
        Installer installer = new Installer(context);
        if (installer.finished()) return;
        JobScheduler jobs = context.getSystemService(JobScheduler.class);
        if (jobs.getPendingJob(JOB_ID) != null) return;
        jobs.schedule(new JobInfo.Builder(JOB_ID, new ComponentName(context, InstallJob.class))
                .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY)
                .setBackoffCriteria(10 * 60 * 1000, JobInfo.BACKOFF_POLICY_EXPONENTIAL)
                .setPersisted(true)
                .build());
    }

    /** Three downloads that were not Tencent's: nothing more is attempted. */
    boolean refused() {
        return state.getInt(REJECTED, 0) >= MAX_REJECTED;
    }

    /** How much of the download is on the device, or -1 before it starts. */
    int downloadedPercent() {
        long total = state.getLong(LENGTH, 0), have = apk().length();
        if (total <= 0 || have <= 0) return -1;
        long percent = have * 100 / total;
        return (int) (percent > 99 ? 99 : percent);
    }

    /**
     * Try as soon as there is a network, with no back-off left over from a
     * failed attempt: what "download now" on the home screen entry means.
     *
     * Also what gets past "this was installed once already" ({@link #DONE}),
     * which is there to keep this application from fetching an installation the
     * reader removed. A tap on that button is the reader asking for it, which is
     * the one thing that rule was never about.
     */
    static void scheduleNow(Context context) {
        Installer installer = new Installer(context);
        if (!installer.installed()) {
            installer.state.edit().remove(DONE).apply();
        }
        context.getSystemService(JobScheduler.class).cancel(JOB_ID);
        schedule(context);
    }

    File apk() {
        return new File(context.getFilesDir(), "weread.apk");
    }

    boolean installed() {
        try {
            context.getPackageManager().getPackageInfo(PACKAGE, 0);
            return true;
        } catch (PackageManager.NameNotFoundException e) {
            return false;
        }
    }

    /** True when there is nothing left to do; records an installation seen. */
    boolean finished() {
        if (state.getBoolean(DONE, false) || state.getInt(REJECTED, 0) >= MAX_REJECTED) return true;
        if (installed()) {
            /* The one place that notices "it is there now" without having put
             * it there, so the one place that can clear up after a success
             * nobody saw the end of: installed() drops the 127 MB file when
             * the session reports success, and a restart between the install
             * and that callback meant nothing ever did -- schedule() asks this
             * question first and leaves, so the attempt that deletes the file
             * was never reached (a Codex review, 2026-10-30). Idempotent: with
             * no file there is nothing to delete, and DONE below makes sure
             * this branch is asked at most once more. */
            if (apk().exists() && !apk().delete()) {
                Log.w(TAG, "could not delete the downloaded file after finding WeRead installed");
            }
            forgetPartial();
            state.edit().putBoolean(DONE, true).apply();
            StatusActivity.retire(context);
            return true;
        }
        return false;
    }

    /**
     * One attempt; `stopped` is polled during the download.
     *
     * One at a time in this process, whoever asked. "Download now" cancels the
     * job and schedules another in the same breath ({@link #scheduleNow}), and
     * cancelling only sets a flag the running attempt polls -- so for as long
     * as the old attempt takes to notice, two of them are live, and both open
     * {@link #apk()} and truncate it (a Codex review, 2026-10-30). The second
     * one waits here instead: the first sees the flag and returns in moments,
     * and if it does not, this attempt gives up and the job is retried rather
     * than writing the same file from two threads.
     */
    Outcome run(StopFlag stopped) {
        boolean held;
        try {
            held = BUSY.tryLock(30, TimeUnit.SECONDS);
        } catch (InterruptedException e) {
            Thread.currentThread().interrupt();
            return Outcome.RETRY;
        }
        if (!held) {
            Log.w(TAG, "another attempt is still running; leaving this one to the retry");
            return Outcome.RETRY;
        }
        try {
            /* The wait above can last half a minute, and what asked for this
             * attempt may have been called off while it waited: a tap that
             * scheduled a newer one, or the scheduler taking the job back. The
             * ordinary cancel check is inside download()'s read loop, which is
             * past a connect of up to 30 seconds and a read of up to 60, and a
             * part that is already whole returns before reaching it at all --
             * so without this, a called-off attempt holds the lock, opens
             * apk() and can go on to install, while the attempt that is still
             * wanted waits out the full 30 seconds and falls back to the
             * ten-minute retry (a Codex review, 2026-10-30). */
            if (stopped.isSet()) {
                Log.w(TAG, "called off while waiting for the other attempt; leaving this one to the retry");
                return Outcome.RETRY;
            }
            return attempt(stopped);
        } finally {
            BUSY.unlock();
        }
    }

    private Outcome attempt(StopFlag stopped) {
        if (finished()) return Outcome.FINISHED;
        /* A session is committed and its result is not in yet. Not FINISHED:
         * while that is true this job is the only thing that can try again, and
         * finishing it leaves an empty queue -- which is how a failed install
         * came to promise "it will try again later" and then not (a Codex
         * review, 2026-10-30; the first fix handed the retry over through a
         * preference, and that only moved the window). RETRY instead, so a
         * retry is queued for as long as a session is outstanding, whichever
         * order the callback and this thread run in. What it costs is one more
         * job run per install: by the time it comes round the result is in,
         * finished() is true, and it returns here in a moment. */
        if (sessionActive()) return Outcome.RETRY;      // ResultReceiver takes over
        try {
            download(stopped);
        } catch (IOException e) {
            Log.w(TAG, "download: " + e);
            if (!stopped.isSet()) notify(context.getString(R.string.retry_later), null, false);
            return Outcome.RETRY;
        }
        /* Called off while the file came down. The download itself is finished
         * and the file stays, so the attempt that replaces this one skips
         * straight to checking it; what must not happen is this attempt going
         * on to install for a job that was taken back. */
        if (stopped.isSet()) return Outcome.RETRY;
        if (finished()) {   // installed by hand meanwhile
            apk().delete();
            forgetPartial();
            return Outcome.FINISHED;
        }
        if (!genuine(apk())) {
            apk().delete();
            forgetPartial();
            int rejected = state.getInt(REJECTED, 0) + 1;
            state.edit().putInt(REJECTED, rejected).apply();
            Log.w(TAG, "rejected download " + rejected + " of " + MAX_REJECTED);
            if (rejected >= MAX_REJECTED) {
                notify(context.getString(R.string.not_genuine_final), null, false);
                return Outcome.FINISHED;
            }
            notify(context.getString(R.string.not_genuine), null, false);
            return Outcome.RETRY;
        }
        /* Before the commit, not after it. installed() can be called back
         * before install() has even returned here, and then this line landed
         * on top of the result that callback had already shown -- "installing"
         * left standing over an installation that had finished or failed (a
         * Codex review, 2026-10-30). Said first, it is only ever replaced by
         * something newer, which is the right way round. install() throwing
         * replaces it below. */
        notify(context.getString(R.string.installing), context.getString(R.string.installing_detail), true);
        try {
            install();
        } catch (IOException e) {
            Log.w(TAG, "install: " + e);
            notify(context.getString(R.string.install_failed), null, false);
            return Outcome.RETRY;
        }
        /* Committed, result not in. The same reason as sessionActive() above:
         * the retry has to be queued before this thread stops being the thing
         * that could queue it. */
        return Outcome.RETRY;
    }

    private void download(StopFlag stopped) throws IOException {
        File file = apk();
        long have = file.exists() ? file.length() : 0;
        // Already the whole file: asking for the bytes after the last one is
        // what a server answers 416 to, and 416 was an IOException that left
        // the length and the ETag in place -- so every retry asked the same
        // impossible question and got the same answer (a Codex review,
        // 2026-10-30). This happens whenever the write finished and the attempt
        // ended before the install did. Nothing to fetch: genuine() and
        // install() decide what becomes of it.
        if (have > 0 && have == state.getLong(LENGTH, 0)) {
            Log.i(TAG, "the file is already " + have + " bytes, which is its length: nothing to download");
            return;
        }
        String etag = state.getString(ETAG, null);
        HttpURLConnection connection = (HttpURLConnection) new URL(SOURCE).openConnection();
        try {
            connection.setConnectTimeout(30_000);
            connection.setReadTimeout(60_000);
            connection.setInstanceFollowRedirects(true);
            if (have > 0 && etag != null) {
                connection.setRequestProperty("Range", "bytes=" + have + "-");
                connection.setRequestProperty("If-Range", etag);
            }
            int code = connection.getResponseCode();
            boolean resumed = code == HttpURLConnection.HTTP_PARTIAL;
            // The range asked for is not there any more -- a shorter file on the
            // server, or a length of ours that was never right. Forget what is
            // remembered so the next attempt asks for the whole thing, rather
            // than repeating a question with one answer.
            if (code == 416) {
                forgetPartial();
                if (!file.delete()) Log.w(TAG, "could not delete the partial file after HTTP 416");
                throw new IOException("HTTP 416; the partial download is forgotten");
            }
            if (code != HttpURLConnection.HTTP_OK && !resumed) throw new IOException("HTTP " + code);
            if (connection.getContentLengthLong() < 0) throw new IOException("no length");
            if (!resumed) {
                /* A whole-file answer replaces what is on the drive, so the
                 * file on the drive stops being what the remembered length
                 * describes -- and it has to stop claiming to be complete
                 * before the new length is written down. Written down first,
                 * a kill in the moment between left the old bytes on the drive
                 * under the new release's length: if the two lengths happened
                 * to match, the next attempt took that remnant for a finished
                 * download, handed it to genuine(), had it refused, and spent
                 * one of the three refusals on a file the server never sent
                 * (a Codex review, 2026-10-30). */
                forgetPartialNow();
                have = 0;
            }
            long total = have + connection.getContentLengthLong();
            byte[] buffer = new byte[64 * 1024];
            long shown = 0;
            try (InputStream in = connection.getInputStream();
                 OutputStream out = new FileOutputStream(file, resumed)) {
                /* After the stream above, which on a whole-file answer has
                 * truncated the file: from here the length describes the file
                 * that is being written and nothing else. */
                state.edit().putString(ETAG, connection.getHeaderField("ETag")).putLong(LENGTH, total).apply();
                int n;
                while ((n = in.read(buffer)) > 0) {
                    if (stopped.isSet()) throw new IOException("stopped");
                    out.write(buffer, 0, n);
                    have += n;
                    long now = SystemClock.elapsedRealtime();
                    // A panel update per change: every 5 s at most.
                    if (now - shown >= 5_000) {
                        shown = now;
                        int percent = (int) (have * 100 / total);
                        notifyProgress(percent, total);
                    }
                }
            }
            if (have != total) throw new IOException("short download: " + have + " of " + total);
        } finally {
            connection.disconnect();
        }
        /* The length is deliberately kept here, where this used to forget it.
         * It is not only resume bookkeeping: it is also how the check at the
         * top of this method knows the file is already whole, and forgetting it
         * the moment the file became whole threw that away exactly when it
         * started to matter. An attempt that was called off between the last
         * byte and the install then came back to a complete 127 MB APK it could
         * not recognise, asked for the whole thing again, and truncated it on
         * the 200 (a Codex review, 2026-10-30) -- the download thrown away by
         * the bookkeeping that was meant to protect it.
         *
         * So ETAG and LENGTH now describe the file at apk() for as long as that
         * file is there, and are forgotten where it goes: rejected (attempt()),
         * found installed by hand (attempt()), or installed by us (installed()).
         * They are harmless if one ever outlives its file, because the check at
         * the top also asks that the file has bytes in it. */
    }

    private void forgetPartial() {
        state.edit().remove(ETAG).remove(LENGTH).apply();
    }

    /**
     * The same, but on the drive before this returns, and it says so if it
     * could not be.
     *
     * apply() writes in the background and is free to skip a version a later
     * write supersedes, so clearing with it is not a barrier on the drive:
     * killed once the new bytes had begun to land, a device could come back to
     * the old length describing a prefix of the new object, take that prefix
     * for a finished download, hand it to genuine(), and spend one of the three
     * refusals on it (a Codex review, 2026-10-30). Throwing rather than
     * carrying on, because replacing the file is the thing that makes the old
     * length a lie, and a length that cannot be unsaid is a reason not to
     * replace it yet.
     *
     * Only at that one transition, which runs on the download thread. The five
     * other calls keep apply(), and none of them is replacing a file with a
     * different one: four come after apk() has already been deleted, so a
     * stale length has nothing left to describe, and the fifth is the 416
     * branch, which clears before deleting but writes no new object, so there
     * is no moment where the length describes bytes that are not its own.
     */
    private void forgetPartialNow() throws IOException {
        if (!state.edit().remove(ETAG).remove(LENGTH).commit()) {
            throw new IOException("the partial download's length could not be cleared");
        }
    }

    /** Tencent's WeRead: its package name, signed with its certificate only. */
    boolean genuine(File file) {
        // Parsing with GET_SIGNATURES verifies the signature of every entry
        // (PackageParser.collectCertificates, APK signature scheme v2 included).
        PackageInfo info = context.getPackageManager()
                .getPackageArchiveInfo(file.getPath(), PackageManager.GET_SIGNATURES);
        if (info == null || !PACKAGE.equals(info.packageName)) return false;
        Signature[] signatures = info.signatures;
        if (signatures == null || signatures.length != 1) return false;
        return CERTIFICATE_SHA256.equals(sha256(signatures[0].toByteArray()));
    }

    private static String sha256(byte[] data) {
        try {
            byte[] digest = MessageDigest.getInstance("SHA-256").digest(data);
            StringBuilder hex = new StringBuilder(digest.length * 2);
            for (byte b : digest) hex.append(String.format("%02x", b & 0xff));
            return hex.toString();
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    /** A committed session still being installed; a stale one is dropped. */
    boolean sessionActive() {
        int id = state.getInt(SESSION, -1);
        if (id < 0) return false;
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionInfo info = installer.getSessionInfo(id);
        if (info != null && info.isActive()) return true;
        // Interrupted, for instance by a restart during the installation.
        if (info != null) installer.abandonSession(id);
        state.edit().remove(SESSION).apply();
        return false;
    }

    private void install() throws IOException {
        File file = apk();
        PackageInstaller installer = context.getPackageManager().getPackageInstaller();
        PackageInstaller.SessionParams params =
                new PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL);
        params.setAppPackageName(PACKAGE);
        params.setSize(file.length());
        // Who the install is for. Launcher3 puts an icon on the home screen
        // for an installed application only when the session says a person
        // asked for it (SessionCommitReceiver: getInstallReason() !=
        // INSTALL_REASON_USER returns without queueing anything). Without this
        // WeRead was installed and had no icon: the entry that installed it
        // was still in its cell and the application was reachable only through
        // the list of all applications (2026-10-20, V60 on the device).
        params.setInstallReason(PackageManager.INSTALL_REASON_USER);
        int id = installer.createSession(params);
        boolean committed = false;
        try (PackageInstaller.Session session = installer.openSession(id)) {
            try (InputStream in = new FileInputStream(file);
                 OutputStream out = session.openWrite("base.apk", 0, file.length())) {
                byte[] buffer = new byte[64 * 1024];
                int n;
                while ((n = in.read(buffer)) > 0) out.write(buffer, 0, n);
                session.fsync(out);
            }
            Intent result = new Intent(context, ResultReceiver.class);
            PendingIntent sender = PendingIntent.getBroadcast(context, id, result,
                    PendingIntent.FLAG_UPDATE_CURRENT);
            state.edit().putInt(SESSION, id).apply();
            session.commit(sender.getIntentSender());
            committed = true;
        } finally {
            if (!committed) {
                installer.abandonSession(id);
                state.edit().remove(SESSION).apply();
            }
        }
    }

    /** The outcome of a committed session (ResultReceiver). */
    void installed(int status, String message) {
        state.edit().remove(SESSION).apply();
        if (status == PackageInstaller.STATUS_SUCCESS) {
            // Said out loud: DONE below closes the branch in finished() that
            // would otherwise come back to this file, so a delete that fails
            // here strands 127 MB for good.
            if (!apk().delete() && apk().exists()) {
                Log.w(TAG, "could not delete the downloaded file after installing");
            }
            forgetPartial();
            state.edit().putBoolean(DONE, true).apply();
            // This is where the installation is known to have succeeded, so
            // this is where the entry gives up its cell. It used to be done
            // only by the status screen, which meant the home screen kept
            // offering to install an application that was already there until
            // somebody opened that screen.
            StatusActivity.retire(context);
            notify(context.getString(R.string.installed), null, false);
            return;
        }
        Log.w(TAG, "install status " + status + ": " + message);
        notify(context.getString(R.string.install_failed), null, false);
        /* That notice says it will try again later, and by the time this runs
         * something already does: the job that committed this session asked to
         * be retried before it let go (attempt(), after install()), so there
         * is one in the queue whichever order the two run in. schedule() is
         * here for the case where there somehow is not -- it does nothing when
         * a job is pending, which is the ordinary case.
         *
         * This is where two earlier attempts at the same problem went: first
         * schedule() alone, which silently did nothing while the committing
         * job was still finishing and then left an empty queue; then a
         * preference the job read on its way out, which only moved the window
         * to between that read and the job's removal (Codex reviews, both
         * 2026-10-30). Queueing the retry before giving up the job is the
         * version with no window in it. */
        schedule(context);
    }

    private NotificationManager notifications() {
        NotificationManager manager = context.getSystemService(NotificationManager.class);
        manager.createNotificationChannel(new NotificationChannel(CHANNEL,
                context.getString(R.string.channel_name), NotificationManager.IMPORTANCE_LOW));
        return manager;
    }

    private void notifyProgress(int percent, long total) {
        String size = context.getString(R.string.megabytes, (int) (total / (1024 * 1024)));
        notifications().notify(NOTIFICATION, new Notification.Builder(context, CHANNEL)
                .setSmallIcon(R.drawable.ic_notify)
                .setContentTitle(context.getString(R.string.downloading))
                .setContentText(context.getString(R.string.downloading_progress, percent, size))
                .setProgress(100, percent, false)
                .setOngoing(true)
                .setOnlyAlertOnce(true)
                .build());
    }

    void notify(String title, String text, boolean ongoing) {
        Notification.Builder builder = new Notification.Builder(context, CHANNEL)
                .setSmallIcon(R.drawable.ic_notify)
                .setContentTitle(title)
                .setOngoing(ongoing)
                .setOnlyAlertOnce(true);
        if (text != null) builder.setContentText(text).setStyle(new Notification.BigTextStyle().bigText(text));
        notifications().notify(NOTIFICATION, builder.build());
    }

    /** Set from the main thread when the job is stopped. */
    static final class StopFlag {
        private volatile boolean set;
        void set() { set = true; }
        boolean isSet() { return set; }
    }
}
