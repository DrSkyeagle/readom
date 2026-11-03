// SPDX-License-Identifier: Apache-2.0
package org.readom.wereadinstaller;

import android.app.job.JobParameters;
import android.app.job.JobService;

/** Runs one attempt while a network is connected; retried with back-off. */
public final class InstallJob extends JobService {
    private Installer.StopFlag stopped;

    @Override
    public boolean onStartJob(final JobParameters params) {
        stopped = new Installer.StopFlag();
        final Installer.StopFlag flag = stopped;
        new Thread("weread-install") {
            @Override
            public void run() {
                // RETRY also means "a session is committed and its result is
                // not in": the attempt asks to be retried before it lets go,
                // so a retry is queued for as long as an install is waiting on
                // its callback, whichever order the two run in (Installer,
                // attempt()). Nothing here has to know about that callback.
                Installer.Outcome outcome = new Installer(InstallJob.this).run(flag);
                if (!flag.isSet()) jobFinished(params, outcome == Installer.Outcome.RETRY);
            }
        }.start();
        return true;
    }

    @Override
    public boolean onStopJob(JobParameters params) {
        // Network lost or the job's time is up: keep the partial file, retry.
        if (stopped != null) stopped.set();
        return true;
    }
}
