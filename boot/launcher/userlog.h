/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * What the release entry leaves on the user store: which kernel log lines may
 * be written there, and how many past runs are kept.
 *
 * Both exist because these files are meant to be sent to a stranger. Someone
 * whose Android did not start attaches the folder to a report, so the folder
 * may not contain what identifies their device, and it may not grow once per
 * tap for ever on a drive that holds their books.
 *
 * The filter is an allow-list, not a list of what to remove: a stock kernel
 * log line can carry the device's MAC address (`usb0: MAC ...`, in 40 of 189
 * logs captured on the device), its serial through the boot command line, or
 * the name of the network it is on, and a list of things to remove is wrong
 * the moment the kernel prints something nobody anticipated. So a line is
 * written only if it comes from one of the parts this project drives, and even
 * then not if it still looks like an identifier.
 */
#ifndef PW3_USERLOG_H
#define PW3_USERLOG_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

/* Whether this kernel log line may be written to the user store. `line` is one
 * line without its newline, as klogctl(3) gives it ("<6>[   32.6] mmc0: ...");
 * the level and timestamp are ignored. */
int pw3_klog_keep(const char *line, size_t len);

/* Whether this line of the report area (device/amazon/pw3/report/ReportArea.h)
 * may be written to the user store. Same destination, same stranger, and the
 * same second gate -- but not the same first one: the allow-list above exists
 * to decide whose line it is, and every line of a report is this project's own
 * by construction, so there is nothing there to decide. What remains is the
 * identifier check, and it remains for the reason it was written: the report
 * names properties and counts today, and the line nobody anticipated is the
 * one it has to catch. */
int pw3_report_keep(const char *line, size_t len);

/* One report, written to the user store as `pw3-android-report-v1 boot=... `,
 * the lines pw3_report_keep allows, and the same name again with the two
 * counts -- the same shape as a filtered kernel log, for the same reason:
 * whoever opens the file is told it is filtered and by how much. `text` is the
 * report's body (ReportArea.h), which is not terminated and whose last line
 * need not end in a newline; an empty line is neither kept nor counted.
 *
 * Returns -1 if any of it failed to reach the stream. The counts are written
 * through whatever happens, so a caller that reports them is reporting what
 * was filtered rather than what was stored. */
int pw3_report_write(FILE *stream, const char *text, size_t bytes, unsigned boot,
		     unsigned *kept, unsigned *left_out);

/* One filtered kernel log, written as its source is read: the source can be a
 * 256 KB ring and only one line of it is held at a time. The file begins with
 * `pw3-kernel-log-filtered-v1 source=...` and ends with the same name and the
 * two counts, so whoever reads it knows it is filtered and by how much. A line
 * longer than the carry is cut, and the remainder faces the allow-list as the
 * next line; a newline and a zero byte both end a line, because the unused part
 * of a raw ring is zeros.
 *
 * `pw3_klog_end` returns -1 if any of it failed to reach the stream, so that
 * the caller decides what that means: a log the preflight owes its folder is
 * worth stopping for, and last session's log -- which is a courtesy and may
 * meet a user store with kilobytes left -- is not. */
struct pw3_klog_out { FILE *stream; unsigned kept, left_out; size_t held; char line[1024]; };
void pw3_klog_begin(struct pw3_klog_out *out, FILE *stream, const char *source);
void pw3_klog_feed(struct pw3_klog_out *out, const char *text, size_t len);
int pw3_klog_end(struct pw3_klog_out *out);

/* Whether `dir`'s store can take `wanted` bytes and still have a megabyte left.
 *
 * Asked before each of the two files a run leaves behind as a courtesy -- the
 * previous session's kernel log, and the report the last Android start wrote --
 * because neither is worth what it would cost to fail: they are written before
 * the installer has checked the space for anything, and the files a start
 * cannot do without are written after them, with no way to carry on when one of
 * those fails. So a store this close to full loses the courtesy and keeps the
 * start. A megabyte is far more than the rest of a run's folder needs.
 *
 *
 * A store that cannot be asked counts as one with no room, for the reason
 * above: not knowing cannot be worth more than the start. `free` is set to what
 * the store said, or to UINT64_MAX when it could not say, so that the run's log
 * records which of the two it was -- a file that is missing from a folder
 * someone sends on should say why it is missing. */
int pw3_store_has_room(const char *dir, uint64_t wanted, uint64_t *free);

/* Hold this run's folder for as long as the run lives, so that no other run's
 * rotation removes it. `fd` is the folder, open for the life of the process;
 * the lock is shared, because holding is all it is for, and the kernel takes it
 * back when the process goes -- so a folder left behind by a run the OOM killer
 * took is free again and is reclaimed normally.
 *
 * It exists because the keep-set cannot tell a folder in use from an old one.
 * The names are times, and a clock that went back gives the run that started
 * later the smaller name, so rotation can pick a folder another run is still
 * writing. Keeping `current` covers the rotating run itself and nothing else.
 *
 * `path` is the same folder by name, and it is asked again once the lock is
 * held: between the folder being opened and this call, another run's rotation
 * can have taken it, read it and removed it, and a shared lock then succeeds
 * on an inode with no name left.
 *
 *  1  held, and the name still leads here. No rotation can take this folder
 *     while this descriptor lives.
 *  0  the lock was not given in five tries, for a reason that is not somebody
 *     holding it -- this kernel allocates the lock before it looks for a
 *     conflict, and that allocation can fail, so this is not evidence that
 *     nobody holds it either. The name still leads here, and nothing holds it
 *     there: exactly the protection every release up to V88 had. Not an error
 *     and not a reason to refuse to install; the caller settles it later,
 *     under the install lock, where no rotation can run (helper.c).
 * -1  a rotation holds this folder, or the name no longer leads here. The
 *     first does not prove the folder will go -- a rotation keeps one it
 *     cannot read -- so it is the caller that decides what to do with it.
 *
 * Returns 1 on this port's own kernel; measured on the device in the run log
 * line `PW3_LOGS_HOLD`. */
int pw3_logs_hold(int fd, const char *path);

/* Keep at most `keep` run folders in `dir` -- counting `current`, the one being
 * written now -- and remove the rest, oldest first. `current` is kept whatever
 * its name sorts as, so a clock that went backwards cannot delete the run in
 * progress; `keep` 0 means the same as 1.
 *
 * A folder goes only when rotation holds the exclusive lock on it, so a folder
 * another run holds (pw3_logs_hold) is never removed, whatever its name sorts
 * as, and neither is one whose lock could not be taken for any other reason --
 * this kernel can refuse a held folder with ENOMEM, so "not EWOULDBLOCK" is no
 * evidence that nobody holds it. Keeping one costs a folder until the next tap
 * rotates again.
 *
 * Only what this entry makes is a candidate: a directory whose name is the
 * launcher's own timestamp shape (digits, 'T', 'Z' and '-', 2 to 80 of them),
 * holding plain files and nothing else. A candidate with a subdirectory, a
 * symbolic link or anything else inside is left alone -- it is then not ours.
 * Nothing is matched by prefix, nothing is followed, nothing recurses.
 *
 * Returns the number of folders removed, or -1 if `dir` could not be read.
 * A folder that cannot be removed is not an error: this is housekeeping, and
 * it must never be the reason a start stops. */
int pw3_logs_rotate(const char *dir, const char *current, unsigned keep);

/* The greatest boot counter carried by any report this launcher has already
 * written under `dir`, in a folder other than `current`, reading the file called
 * `leaf` in each. 1 with *boot set, 0 when no report of ours has a counter, -1
 * when the directory could not be read through -- which is not the same answer
 * as there being nothing in it.
 *
 * It exists so that a tap can say something about the tap before it. Nothing in
 * the folder itself could: the folder is made before Android runs and nothing
 * writes to it afterwards, so a failed start and a successful one leave the same
 * folder behind -- which is why, on 2026-10-21, whether V65 had started was
 * known only because the person said so. The report area's boot counter does go
 * up, and the previous tap's copy of it is already on the store.
 *
 * It takes the greatest counter rather than the counter of the most recent
 * folder, and so needs no ordering at all. Ordering by name was the first
 * version of this, on the reading that the name is a timestamp; a clock that
 * goes back makes the greatest name an older run, and a start that failed then
 * compares against that older run's smaller counter and reads as one that
 * worked. What the caller wants is only the highest number seen so far.
 *
 * What that number can and cannot settle is in docs/known-issues.md: it rises
 * when Android reached pw3-boot-report and wrote, which is neither the same as
 * the framework coming up nor, when it has not risen, proof that nothing ran. */
int pw3_logs_report_max(const char *dir, const char *current, const char *leaf, unsigned *boot);

/* The one word the entry prints about that comparison, given what
 * pw3_logs_report_max returned and the counter read this time:
 *
 *   "reported"            a report newer than any before it. Android got as far
 *                         as pw3-boot-report and its write succeeded.
 *   "no_new_report"       the same counter as before. Either nothing ran, or
 *                         something ran and did not get to write -- the counter
 *                         cannot tell those apart (docs/known-issues.md).
 *   "no_previous_report"  nothing to compare against; the first tap that found
 *                         a report.
 *   "unknown"             the folders could not be read through, so no
 *                         comparison was made. Not the same as "nothing there".
 *
 * It is a function rather than four printfs because it is the judgement, and the
 * first version of it printed "started" and "not_started" -- asserting the two
 * things the counter does not settle -- with no test able to reach it. */
const char *pw3_logs_start_word(int found, unsigned before, unsigned now);

#endif
