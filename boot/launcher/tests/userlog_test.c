/* SPDX-License-Identifier: GPL-2.0-only */
/* What the release entry leaves on the user store (userlog.c): which kernel log
 * lines it writes there, and which run folders it removes.
 *
 * Both are worth a test on this machine rather than on the device. The filter
 * decides whether someone's MAC address ends up in a report, and the rotation
 * deletes directories on the drive that holds the user's books -- a mistake in
 * either is found here or by them.
 *
 * Usage: userlog_test   (no arguments; it makes its own directories under the
 *                        working directory) */
#include "../userlog.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { printf("PW3_USERLOG_FAIL line=%u\n", __LINE__); return 1; } } while (0)

static int keep(const char *line)
{
	return pw3_klog_keep(line, strlen(line));
}

static void make_run(const char *root, const char *name, const char *const *files)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, name);
	mkdir(path, 0700);
	for (unsigned i = 0; files[i]; i++) {
		snprintf(path, sizeof(path), "%s/%s/%s", root, name, files[i]);
		FILE *f = fopen(path, "w");
		if (f) {
			fputs("evidence\n", f);
			fclose(f);
		}
	}
}

static int present(const char *root, const char *name)
{
	char path[512];
	struct stat st;
	snprintf(path, sizeof(path), "%s/%s", root, name);
	return !stat(path, &st);
}

static int test_filter(void)
{
	/* The parts this project drives are written. */
	CHECK(keep("<6>[   32.123456] PM: Syncing filesystems ... done."));
	CHECK(keep("<6>[   32.123456] suspend debug: Waiting for 5 seconds."));
	CHECK(keep("<6>[ 1707.376493] fsl-usb2-udc: bind to driver g_file_storage"));
	CHECK(keep("<6>[    5.000000] g_file_storage gadget: high speed config #1"));
	CHECK(keep("<4>[    8.000000] mmcblk0: error -110 transferring data"));
	CHECK(keep("<6>[    1.234567] EXT3-fs (mmcblk0p1): using internal journal"));
	CHECK(keep("<6>[    2.000000] mxc_epdc_fb: [1] Sending update. waveform:0x4 (gl16)"));
	CHECK(keep("<6>[   10.000000] KERNEL: I pmic:charger chgina::charger connected"));
	CHECK(keep("<6>[   11.000000] add wake up source irq 104"));
	CHECK(keep("<6>[   12.000000] otg udc vbus rising wakeup"));

	/* What this project prints itself, under Android: the cable and the
	 * pull-up (patch 1046), the gadget, and whatever a later diagnostic
	 * prints under the same prefix. */
	CHECK(keep("<6>[   13.000000] pw3-usb: cable in"));
	CHECK(keep("<6>[   13.100000] pw3-usb: pull-up on: driver 1 connect 1 vbus 1 session 1"));
	CHECK(keep("<3>[   14.000000] pw3-init-check: data target differs from configured layout"));
	CHECK(keep("<6>[   16.000000] android_usb gadget: high speed config #1: android_usb"));
	/* The prefix is not a licence to print an identifier: second gate. */
	CHECK(!keep("<3>[   17.000000] pw3-usb: iSerial 0123456789abcdef"));
	/* And not a licence to print what an application wrote: the diagnostic
	 * boot report relays logcat, so the prefix does not carry it. */
	CHECK(!keep("<3>[   15.000000] pw3-boot-report: sys.usb.state=adb"));
	CHECK(!keep("<3>[   15.100000] pw3-boot-report: android_error ActivityManager: Unable to open /mnt/us/documents/notes.txt"));
	/* But the same program's own reasons for not having written the report
	 * are fixed text, and they are the reasons a reader hands on. They print
	 * under a tag of their own because this one is matched by prefix: tagged
	 * `pw3-boot-report`, the diagnostics added on 2026-10-21 were collected
	 * here and then dropped, which is the gap they were added to close
	 * (BootReport.cpp, KernelLine; tools/tests/test_klog_tags.py). */
	CHECK(keep("<3>[   15.200000] pw3-report-area: report area cannot be opened for writing: Permission denied"));
	CHECK(keep("<3>[   15.300000] pw3-report-area: /dev/block/dm-3 unusable: Permission denied"));
	CHECK(keep("<3>[   15.400000] pw3-report-area: report area not written at=write errno=5"));

	/* A kernel in trouble is written whatever printed it. */
	CHECK(keep("<0>[   40.000000] Unable to handle kernel paging request at virtual address deadbee0"));

	/* SELinux denials. V65 installed and then refused to start because
	 * pw3_init_check had no rule for the report area's device label; the kernel
	 * said so and this filter dropped the line, so the folder handed on said
	 * nothing. Kept now -- bounded so that none of the user's own names can
	 * ride along (userlog.c, denial()). */
	/* The line the device actually printed on 2026-10-21, verbatim. */
	CHECK(keep("<5>[   31.666703] type=1400 audit(1792543841.720:4): avc:  denied  { getattr } "
		   "for  pid=1102 comm=\"pw3-init-check\" path=\"/dev/block/dm-3\" dev=tmpfs ino=973 "
		   "scontext=u:r:pw3_init_check:s0 tcontext=u:object_r:pw3_report_block_device:s0 "
		   "tclass=blk_file permissive=0"));
	CHECK(keep("<5>[    9.200000] avc:  denied  { read } for  pid=200 comm=\"pw3-render\" "
		   "path=\"/sys/class/graphics/fb0/state\" scontext=u:r:pw3_render:s0 "
		   "tcontext=u:object_r:sysfs:s0 tclass=file permissive=0"));
	/* No path and no name: nothing of theirs is in it. */
	CHECK(keep("<5>[    9.300000] avc:  denied  { sys_admin } for  pid=1102 comm=\"pw3-init-check\" "
		   "capability=21 scontext=u:r:pw3_init_check:s0 tcontext=u:r:pw3_init_check:s0 "
		   "tclass=capability permissive=0"));
	/* A path in the user store is dropped whole rather than rewritten. */
	CHECK(!keep("<5>[    9.400000] avc:  denied  { read } for  pid=300 comm=\"pw3-private-assets\" "
		    "path=\"/mnt/us/documents/\345\221\250\346\230\223\344\274\240.azw3\" "
		    "scontext=u:r:pw3_private_assets:s0 tcontext=u:object_r:vfat:s0 "
		    "tclass=file permissive=0"));
	/* A bare name has nothing bounding it: a book's title looks like this. */
	CHECK(!keep("<5>[    9.500000] avc:  denied  { search } for  pid=300 comm=\"pw3-storage\" "
		    "name=\"my-notes\" dev=vfat ino=42 scontext=u:r:pw3_storage:s0 "
		    "tcontext=u:object_r:vfat:s0 tclass=dir permissive=0"));
	/* Somebody else's domain is not ours to hand on. */
	CHECK(!keep("<5>[    9.600000] avc:  denied  { read } for  pid=900 comm=\"Binder:900_2\" "
		    "path=\"/dev/block/dm-3\" scontext=u:r:system_server:s0 "
		    "tcontext=u:object_r:pw3_report_block_device:s0 tclass=blk_file permissive=0"));
	/* And the identifier gate still has the last word. */
	CHECK(!keep("<5>[    9.700000] avc:  denied  { read } for  pid=400 comm=\"pw3-wifi\" "
		    "path=\"/sys/class/net/wlan0/address\" mac=01:23:45:67:89:ab "
		    "scontext=u:r:pw3_wifi:s0 tcontext=u:object_r:sysfs:s0 tclass=file permissive=0"));
	CHECK(keep("<4>[   41.000000] page allocation failure: order:9, mode:0xd0"));
	CHECK(keep("<4>[   42.000000] Out of memory: Kill process 1 (init) score 0"));
	CHECK(keep("<4>[   43.000000] BUG: scheduling while atomic"));
	CHECK(keep("<0>[   44.000000] Kernel panic - not syncing: Attempted to kill init!"));

	/* The dump under those headings. V78's last start left "Kernel panic"
	 * and "Backtrace:" in the folder with nothing after either of them, and
	 * a kernel ring is 32 KB: by the time anybody looked, the 385 dropped
	 * lines were the only record of where it died and they were gone. */
	CHECK(keep("<1>[10225.645855] Unable to handle kernel NULL pointer dereference at virtual address 00000088"));
	CHECK(keep("<1>[10225.645900] pgd = c0004000"));
	CHECK(keep("<4>[10225.646100] CPU: 0    Not tainted  (3.0.35-lab126-pw3-android81 #1)"));
	CHECK(keep("<4>[10225.646120] Modules linked in: mxc_epdc_fb ath6kl_sdio"));
	CHECK(keep("<4>[10225.646150] PC is at mxcfb_resume+0x18/0x74"));
	CHECK(keep("<4>[10225.646160] LR is at dpm_run_callback+0x34/0x78"));
	CHECK(keep("<4>[10225.646170] pc : [<c0304a18>]    lr : [<c02b1234>]    psr: 60000193"));
	CHECK(keep("<4>[10225.646180] sp : c0a3fd80  ip : 00000000  fp : c0a3fdac"));
	CHECK(keep("<4>[10225.646190] r10: 00000000  r9 : c0a3e000  r8 : c08d1b40"));
	CHECK(keep("<4>[10225.646200] r7 : 00000000  r6 : c08d1b40  r5 : 00000000  r4 : c08d1b08"));
	CHECK(keep("<4>[10225.646210] r3 : 00000000  r2 : 00000000  r1 : 00000001  r0 : 00000000"));
	CHECK(keep("<4>[10225.646220] Flags: nZCv  IRQs off  FIQs on  Mode SVC_32  ISA ARM  Segment kernel"));
	CHECK(keep("<4>[10225.646230] Control: 10c53c7d  Table: 80004019  DAC: 00000015"));
	CHECK(keep("<4>[10225.646478] [<c00144a0>] (unwind_backtrace+0x0/0xf8) from [<c05f8e0c>] (panic+0x74/0x1a4)"));
	/* Still not the process, nor the instruction bytes: both can be an
	 * application's rather than the kernel's. */
	CHECK(!keep("<4>[10225.646240] Process com.example.reader (pid: 1234, stack limit = 0xc0a3e2f0)"));
	CHECK(!keep("<4>[10225.646500] Code: e1a0c00d e92dd800 e24cb004 e24dd008 (e5903088)"));

	/* What identifies the device is not, and that is the whole reason this
	 * exists: the first of these is in 20 of the 201 logs captured on it. */
	CHECK(!keep("<6>[ 1707.376493] usb0: MAC 01:23:45:67:89:ab"));
	CHECK(!keep("<6>[ 1707.376504] usb0: HOST MAC 01:23:45:67:89:ac"));
	CHECK(!keep("<5>[    0.000000] Kernel command line: console=ttymxc0 serial=0123456789abcdef"));
	CHECK(!keep("<6>[   32.659546] ath6kl: connected to ssid HomeNetwork"));
	CHECK(!keep("<6>[   33.000000] cfg80211: Calling CRDA for country: US"));

	/* An allowed part printing an identifier anyway: the second gate. */
	CHECK(!keep("<6>[    5.000000] g_file_storage gadget: serial=0123456789abcdef"));
	CHECK(!keep("<6>[    5.000000] g_file_storage gadget: Inquiry string 01:23:45:67:89:ab"));
	CHECK(!keep("<6>[    5.000000] mmcblk0: eui 0123456789abcd"));
	CHECK(keep("<6>[    5.000000] mmcblk0: 0123456789a is eleven hex, not twelve"));

	/* The noise it is there to leave out, measured on the device: the Wi-Fi
	 * card's bus, the charger poll, the stock system's own crashes. */
	CHECK(!keep("<6>[   32.000000] mmc1: queuing unknown CIS tuple 0x80 (3 bytes)"));
	CHECK(!keep("<6>[   32.000000] max77696_uic_isr: cancel: 0"));
	CHECK(!keep("<4>[   50.000000] Code: e3a03000 e5833000 (e5933000) e3530000"));
	CHECK(!keep("<4>[   51.000000] Thread-73 (5550) segfault at 0 ip 00000000"));
	CHECK(!keep("<6>[   52.000000] CCM: vaddr f40c4000, va 0xf40c4200 pa 0x20110c3"));

	/* A line with no level or timestamp, and the empty line. */
	CHECK(keep("PM: suspend entry"));
	CHECK(!keep(""));
	CHECK(!keep("<6>[   32.000000] "));
	/* A truncated line cannot read past its length. */
	CHECK(!pw3_klog_keep("<6>[   32.0", 11));
	CHECK(!pw3_klog_keep("PM", 2));
	return 0;
}

/* The streaming writer: a source arrives in chunks of whatever size the read
 * returned, and a line split across two of them is still one line. */
static int test_stream(void)
{
	char text[4096];
	struct pw3_klog_out out;
	FILE *f = fmemopen(text, sizeof(text), "w");
	CHECK(f != NULL);
	pw3_klog_begin(&out, f, "test");
	pw3_klog_feed(&out, "<6>[    1.0] PM: suspend ent", 28);
	pw3_klog_feed(&out, "ry\n<6>[    2.0] mmc1: noise\n<6>[    3.0] mmc0: kept\n", 52);
	/* A ring's unused tail is zeros, and each zero ends a line. */
	pw3_klog_feed(&out, "<6>[    4.0] sdhci: last\0\0\0\0", 28);
	CHECK(pw3_klog_end(&out) == 0);
	CHECK(fclose(f) == 0);
	CHECK(out.kept == 3 && out.left_out == 1);
	CHECK(strstr(text, "pw3-kernel-log-filtered-v1 source=test\n") == text);
	CHECK(strstr(text, "PM: suspend entry\n") != NULL);
	CHECK(strstr(text, "mmc0: kept\n") != NULL);
	CHECK(strstr(text, "sdhci: last\n") != NULL);
	CHECK(!strstr(text, "mmc1"));
	CHECK(strstr(text, "\npw3-kernel-log-filtered-v1 kept=3 left_out=1\n") != NULL);

	/* A line longer than the carry is cut, and both halves are judged on
	 * their own: the first keeps its tag, the remainder has none. */
	char longer[2400];
	memset(longer, 'x', sizeof(longer));
	memcpy(longer, "<6>[    5.0] mmc0: ", 19);
	f = fmemopen(text, sizeof(text), "w");
	CHECK(f != NULL);
	pw3_klog_begin(&out, f, "cut");
	pw3_klog_feed(&out, longer, sizeof(longer));
	CHECK(pw3_klog_end(&out) == 0);
	CHECK(fclose(f) == 0);
	CHECK(out.kept == 1 && out.left_out == 2);

	/* A stream that cannot take it all says so, and does not pretend. */
	char tiny[64];
	f = fmemopen(tiny, sizeof(tiny), "w");
	CHECK(f != NULL);
	pw3_klog_begin(&out, f, "a source name long enough to fill this stream by itself");
	pw3_klog_feed(&out, "<6>[    6.0] mmc0: and a line after it\n", 39);
	CHECK(pw3_klog_end(&out) == -1);
	fclose(f);
	return 0;
}

/* The report area's lines on their way to the same folder (pw3_report_write).
 * The allow-list has nothing to say about them, so what is tested is the gate
 * that does: the identifier check, and the line arithmetic around it. */
static int test_report(void)
{
	char text[4096];
	unsigned kept, left_out;
	const char *body =
		"boot=7 uptime_ms=41230\n"		/* ours, and nothing to hide */
		"sys.usb.config=adb\n"
		"\n"					/* an empty line is neither */
		"wifi.ssid=the-cafe\n"			/* names a network */
		"gadget.mac=12:34:56:78:9a:bc\n"	/* a MAC address */
		"other.mac=12-34-56-78-9a-bc\n"		/* the same, written with hyphens */
		"hash=0123456789abcdef\n"		/* a long run of hex */
		"epdc.full=19 epdc.partial=562";	/* no newline at the end */
	FILE *f = fmemopen(text, sizeof(text), "w");
	CHECK(f != NULL);
	CHECK(pw3_report_write(f, body, strlen(body), 7, &kept, &left_out) == 0);
	CHECK(fclose(f) == 0);
	CHECK(kept == 3 && left_out == 4);
	CHECK(strstr(text, "pw3-android-report-v1 boot=7 bytes=") == text);
	CHECK(strstr(text, "\nboot=7 uptime_ms=41230\n") != NULL);
	CHECK(strstr(text, "\nsys.usb.config=adb\n") != NULL);
	CHECK(strstr(text, "\nepdc.full=19 epdc.partial=562\n") != NULL);
	CHECK(!strstr(text, "the-cafe") && !strstr(text, "9a:bc") && !strstr(text, "9a-bc") &&
	      !strstr(text, "0123456789abcdef"));
	CHECK(strstr(text, "\npw3-android-report-v1 kept=3 left_out=4\n") != NULL);

	/* A report of nothing: the two lines that say so, and no third. */
	f = fmemopen(text, sizeof(text), "w");
	CHECK(f != NULL);
	CHECK(pw3_report_write(f, "", 0, 0, &kept, &left_out) == 0);
	CHECK(fclose(f) == 0);
	CHECK(kept == 0 && left_out == 0);
	CHECK(strstr(text, "bytes=0\npw3-android-report-v1 kept=0 left_out=0\n") != NULL);

	/* Judged line by line, not as a whole: one bad line does not take the
	 * others with it. The body is on the heap and exactly `bytes` long, with
	 * no terminator after it, so a read past the end is a fault the
	 * sanitizers this is built with report rather than a byte that happens to
	 * be readable. */
	{
		const char *whole = "a=1\nserial=X\nb=2";
		char *body16 = malloc(16);
		CHECK(body16 != NULL);
		memcpy(body16, whole, 16);
		f = fmemopen(text, sizeof(text), "w");
		CHECK(f != NULL);
		CHECK(pw3_report_write(f, body16, 16, 9, &kept, &left_out) == 0);
		CHECK(fclose(f) == 0);
		CHECK(kept == 2 && left_out == 1);
		CHECK(strstr(text, "\nb=2\n") != NULL && !strstr(text, "serial"));
		free(body16);
	}

	/* A full user store refuses it, and that is reported, not hidden. */
	char tiny[32];
	f = fmemopen(tiny, sizeof(tiny), "w");
	CHECK(f != NULL);
	CHECK(pw3_report_write(f, "a=1\nb=2\n", 8, 4294967295u, &kept, &left_out) == -1);
	fclose(f);
	return 0;
}

/* The room a courtesy file asks for before it is written. The free space of the
 * filesystem this test runs on is not known, so what is checked is the two
 * answers that do not depend on it. */
static int test_room(void)
{
	uint64_t free_bytes = 0;
	CHECK(pw3_store_has_room(".", 0, &free_bytes) == 1);
	CHECK(free_bytes > 0 && free_bytes != UINT64_MAX);
	CHECK(pw3_store_has_room(".", (uint64_t)1 << 62, &free_bytes) == 0);
	/* A store that cannot be asked counts as one with no room, and says so
	 * with a number no store can have. */
	CHECK(pw3_store_has_room("no-such-directory", 0, &free_bytes) == 0);
	CHECK(free_bytes == UINT64_MAX);
	return 0;
}

static int test_rotate(const char *root)
{
	static const char *const files[] = {"launcher.log", "core-return.txt", NULL};
	static const char *const names[] = {"20261001T100000-100", "20261002T100000-200",
					    "20261003T100000-300", "20261004T100000-400",
					    "20261005T100000-500", "20261006T100000-600"};
	mkdir(root, 0700);
	for (unsigned i = 0; i < 6; i++)
		make_run(root, names[i], files);

	/* Six runs, five kept: the oldest goes, with what was inside it. */
	CHECK(pw3_logs_rotate(root, names[5], 5) == 1);
	CHECK(!present(root, names[0]));
	for (unsigned i = 1; i < 6; i++)
		CHECK(present(root, names[i]));
	/* Again with nothing to do. */
	CHECK(pw3_logs_rotate(root, names[5], 5) == 0);

	/* The run writing now is kept whatever its name sorts as: a clock that
	 * went backwards must not delete the folder being written. Two kept of
	 * six means this one and the newest of the others. */
	make_run(root, "20250101T000000-1", files);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 2) == 4);
	CHECK(present(root, "20250101T000000-1"));
	CHECK(present(root, names[5]));
	CHECK(!present(root, names[4]) && !present(root, names[1]));

	/* Nothing else on the drive is a candidate, however close the name.
	 *
	 * `2026` and the two short ones are the cases this missed until
	 * 2026-10-21: ownership was decided by the alphabet of the name, so a
	 * folder of nothing but digits was taken for one of ours, and four older
	 * folders were enough for rotation to delete it and every plain file a
	 * reader had put inside it. The names below are checked for their shape
	 * now -- fifteen characters of timestamp, a hyphen, and digits. */
	static const char *const safe[] = {"documents", "20261007T100000-700x", "books",
					   "20261008-800.txt", "2026", "20261007", "20261007T100000",
					   "20261007T100000-", "2026-10-07T10-00-00",
					   /* Each of these differs from one of ours in exactly
					    * one position, so each pins the check for that
					    * position on its own. `2026-10-07T10-00-00` does
					    * not: it is refused at its fourth character, so it
					    * never reaches the ninth, and with the digit check
					    * taken out of ours() it was still refused and this
					    * test still passed. */
					   "20261007X100000-700",    /* the T */
					   "2026100XT100000-700",    /* a digit in the date */
					   "20261007T1000X0-700",    /* a digit in the time */
					   "20261007T100000X700",    /* the hyphen */
					   "20261007T100000-70X"};   /* a digit in the pid */
	const unsigned safe_count = sizeof(safe) / sizeof(safe[0]);
	for (unsigned i = 0; i < safe_count; i++)
		make_run(root, safe[i], files);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 1) == 1);
	for (unsigned i = 0; i < safe_count; i++)
		CHECK(present(root, safe[i]));
	/* And what was inside them, which is the part that was lost. */
	for (unsigned i = 0; i < safe_count; i++) {
		char kept[512];
		struct stat st;
		snprintf(kept, sizeof(kept), "%s/%s/%s", root, safe[i], files[0]);
		CHECK(!stat(kept, &st) && st.st_size > 0);
	}
	CHECK(present(root, "20250101T000000-1") && !present(root, names[5]));

	/* A folder with our name but something else inside is not ours. */
	char path[512];
	make_run(root, "20261009T100000-900", files);
	snprintf(path, sizeof(path), "%s/%s/inside", root, "20261009T100000-900");
	mkdir(path, 0700);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 1) == 0);
	CHECK(present(root, "20261009T100000-900"));
	rmdir(path);

	/* A symbolic link where a run folder would be is left alone. */
	snprintf(path, sizeof(path), "%s/20261010T100000-999", root);
	CHECK(symlink("/", path) == 0 || errno == EEXIST);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 1) == 1);
	CHECK(!present(root, "20261009T100000-900"));
	struct stat st;
	CHECK(!lstat(path, &st) && S_ISLNK(st.st_mode));
	CHECK(unlink(path) == 0);

	/* A folder another run is holding is left alone, whatever its name sorts
	 * as -- which is the whole point: the names are times, and a clock that
	 * went back gives the run that started later the smaller name, so the
	 * keep-set cannot tell a folder in use from an old one. The name chosen
	 * here sorts below every other folder, so by name it is first to go. */
	make_run(root, "20200101T000000-1", files);
	snprintf(path, sizeof(path), "%s/20200101T000000-1", root);
	int held = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(held >= 0);
	CHECK(pw3_logs_hold(held, path) == 1);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 1) == 0);
	CHECK(present(root, "20200101T000000-1"));
	/* And once that run is gone -- the kernel takes the lock back when the
	 * file description closes, which is what happens to a run the OOM killer
	 * takes -- the folder is reclaimed as any other. */
	CHECK(close(held) == 0);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 1) == 1);
	CHECK(!present(root, "20200101T000000-1"));

	/* Holding the folder is not the whole answer: a rotation can have taken
	 * it between the open and the lock, and the lock then succeeds on an inode
	 * with no name left. The name is asked again under the lock, and a folder
	 * that is no longer there says so here rather than at the first file this
	 * run owes it. */
	make_run(root, "20200102T000000-1", files);
	snprintf(path, sizeof(path), "%s/20200102T000000-1", root);
	held = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(held >= 0);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 1) == 1);
	CHECK(!present(root, "20200102T000000-1"));
	CHECK(pw3_logs_hold(held, path) == -1);
	CHECK(close(held) == 0);

	/* And a name that leads somewhere else is the same answer: the folder this
	 * run opened is not the folder the name now means. */
	make_run(root, "20200103T000000-1", files);
	snprintf(path, sizeof(path), "%s/20200103T000000-1", root);
	held = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	CHECK(held >= 0);
	CHECK(pw3_logs_hold(held, root) == -1);
	CHECK(pw3_logs_hold(held, path) == 1);
	CHECK(close(held) == 0);
	CHECK(pw3_logs_rotate(root, "20250101T000000-1", 1) == 1);

	/* A directory that is not there at all. */
	CHECK(pw3_logs_rotate("no-such-directory", "x", 5) == -1);
	return 0;
}

/* A folder with a report in it whose header says `boot`, written the way
 * pw3_report_write writes one. `tail` is appended after the counter so that a
 * header cut short can be built as well as a whole one. */
static void make_report(const char *root, const char *name, const char *tail)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", root, name);
	mkdir(path, 0700);
	snprintf(path, sizeof(path), "%s/%s/android-report.txt", root, name);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(tail, f);
		fclose(f);
	}
}

static void remove_report(const char *root, const char *name)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s/android-report.txt", root, name);
	unlink(path);
	snprintf(path, sizeof(path), "%s/%s", root, name);
	rmdir(path);
}

/* The greatest counter any earlier tap recorded, and the word the entry prints
 * about it. Between them these are what lets a tap say anything at all about the
 * tap before it: the folder itself is written before Android runs and nothing
 * writes to it afterwards, so a failed start and a successful one leave folders
 * that read exactly alike.
 *
 * The version of this before 2026-10-21 found the folder with the greatest name
 * and read its counter, and this test only checked which folder was chosen -- it
 * never read a counter and never reached the judgement, so the judgement could
 * have been inverted with every check still passing. */
static int test_report_max(const char *parent)
{
	static const char *const files[] = {"launcher.log", NULL};
	unsigned boot = 123;
	/* Small enough that appending a folder name to it cannot fill `path`
	 * below: gcc checks that and the host compiler does not. */
	char root[64];
	/* Its own directory: test_rotate leaves folders of its own behind in the
	 * one it was given, and "nothing recorded a counter yet" has to start
	 * from a place where nothing did. */
	snprintf(root, sizeof(root), "%s-reports", parent);
	mkdir(root, 0700);

	/* Nothing to compare with yet, and a folder of ours with no report in it
	 * is still nothing to compare with. */
	make_run(root, "20261101T100000-100", files);
	CHECK(pw3_logs_report_max(root, "20261101T100000-100", "android-report.txt", &boot) == 0);
	make_run(root, "20261102T100000-200", files);
	CHECK(pw3_logs_report_max(root, "20261101T100000-100", "android-report.txt", &boot) == 0);

	/* The greatest counter, not the counter of the greatest name. A clock
	 * that went back puts the newer run under the smaller name, and taking
	 * the newest name read the older run's smaller counter -- which made a
	 * start that had failed look like one that worked. */
	make_report(root, "20261103T100000-300", "pw3-android-report-v1 boot=7 bytes=10\nbody\n");
	make_report(root, "20250101T000000-1", "pw3-android-report-v1 boot=9 bytes=10\nbody\n");
	CHECK(pw3_logs_report_max(root, "20261104T100000-400", "android-report.txt", &boot) == 1);
	CHECK(boot == 9);
	/* The folder being written is never counted, whatever its name sorts as. */
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 7);

	/* A header cut short is no counter at all. `boot=1` of `boot=12` parsed
	 * as 1 before the whole line was required, and a start that had failed
	 * then compared 1 against 12 and read as one that worked. The number here
	 * is larger than any whole one on the drive, so that accepting it changes
	 * the answer -- with a smaller number this check passed either way. */
	make_report(root, "20261105T100000-500", "pw3-android-report-v1 boot=99");
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 7);
	/* Nor is a header whose second field is cut off, or whose line never ends --
	 * and the two are separate cases. With only the newline required, both of
	 * the cases below were refused for the missing newline and the `bytes`
	 * field was never pinned, so a parse that ignored it entirely passed. */
	make_report(root, "20261105T100000-500", "pw3-android-report-v1 boot=99 bytes=\nbody\n");
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 7);
	make_report(root, "20261105T100000-500", "pw3-android-report-v1 boot=99\nbody\n");
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 7);
	make_report(root, "20261105T100000-500", "pw3-android-report-v1 boot=99 bytes=");
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 7);
	make_report(root, "20261105T100000-500", "pw3-android-report-v1 boot=99 bytes=10");
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 7);
	/* And a whole one is taken. */
	make_report(root, "20261105T100000-500", "pw3-android-report-v1 boot=12 bytes=10\nbody\n");
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 12);

	/* Nothing else on the drive is counted, however close the name, and a
	 * folder whose name is only digits is somebody else's. */
	static const char *const safe[] = {"documents", "20261106T100000-600x", "2026",
					   "20261107T100000", "20261108T100000-", NULL};
	for (unsigned i = 0; safe[i]; i++)
		make_report(root, safe[i], "pw3-android-report-v1 boot=4000 bytes=10\nbody\n");
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 12);

	/* A plain file with one of our names is not a folder. */
	char path[512];
	snprintf(path, sizeof(path), "%s/20261109T100000-900", root);
	FILE *f = fopen(path, "w");
	CHECK(f != NULL);
	if (f)
		fclose(f);
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 12);
	CHECK(unlink(path) == 0);

	/* Nor is a symbolic link, even one of our names pointing at a folder that
	 * has a report in it. The plain file above does not show this: the open of
	 * `<file>/android-report.txt` fails anyway, so skipping the directory check
	 * entirely still gave the right answer and this test still passed. A link
	 * is followed by that open, so only the check keeps it out. */
	make_report(root, "20261110T100000-910", "pw3-android-report-v1 boot=5000 bytes=10\nbody\n");
	snprintf(path, sizeof(path), "%s/20261111T100000-920", root);
	CHECK(symlink("20261110T100000-910", path) == 0 || errno == EEXIST);
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	/* 5000 because the folder it points at is itself one of ours and counts;
	 * the link must not make it count twice, and must not count when the
	 * folder it names is not ours. */
	CHECK(boot == 5000);
	CHECK(unlink(path) == 0);
	remove_report(root, "20261110T100000-910");
	snprintf(path, sizeof(path), "%s/20261112T100000-930", root);
	CHECK(symlink("documents", path) == 0 || errno == EEXIST);
	CHECK(pw3_logs_report_max(root, "20250101T000000-1", "android-report.txt", &boot) == 1);
	CHECK(boot == 12);
	CHECK(unlink(path) == 0);

	/* A directory that is not there is not an empty one: the caller has to
	 * be able to tell "no counter" from "could not look". */
	CHECK(pw3_logs_report_max("no-such-directory", "x", "android-report.txt", &boot) == -1);
	CHECK(pw3_logs_report_max(root, "x", "a/b", &boot) == -1);
	CHECK(pw3_logs_report_max(root, "x", "android-report.txt", NULL) == -1);

	/* The shape ours() accepts is the shape the launcher writes, and the two
	 * are held together here rather than by both having been read once.
	 * Tightening ours() can only go wrong in two directions: too loose, which
	 * deletes somebody else's folder (above), or too tight, which stops
	 * rotation recognising its own and fills the drive instead. The names
	 * below are built the way launcher-template.sh builds them -- strftime
	 * "%Y%m%dT%H%M%S" and the process id -- over dates and pid widths that
	 * bracket what it can produce. tools/tests/test_launcher_name.py holds the
	 * other half: that the shell still uses that format. */
	{
		static const time_t when[] = {0, 1767225599, 4102444800, 1709164800, 2524608000};
		static const long pids[] = {1, 9, 10, 99999, 4194304};
		char built[81];
		for (unsigned w = 0; w < sizeof(when) / sizeof(when[0]); w++) {
			struct tm parts;
			char stamp[32];
			CHECK(gmtime_r(&when[w], &parts) != NULL);
			CHECK(strftime(stamp, sizeof(stamp), "%Y%m%dT%H%M%S", &parts) == 15);
			for (unsigned p = 0; p < sizeof(pids) / sizeof(pids[0]); p++) {
				snprintf(built, sizeof(built), "%s-%ld", stamp, pids[p]);
				make_report(root, built, "pw3-android-report-v1 boot=3 bytes=10\nbody\n");
				CHECK(pw3_logs_report_max(root, "x", "android-report.txt", &boot) == 1);
				CHECK(boot == 12);
				/* Counted, which is what being ours means here: without the
				 * folder the answer would still be 12, so the folder is given
				 * the greatest counter on the drive and has to win. */
				make_report(root, built, "pw3-android-report-v1 boot=4001 bytes=10\nbody\n");
				CHECK(pw3_logs_report_max(root, "x", "android-report.txt", &boot) == 1);
				CHECK(boot == 4001);
				remove_report(root, built);
			}
		}
		CHECK(pw3_logs_report_max(root, "x", "android-report.txt", &boot) == 1);
		CHECK(boot == 12);
	}

	/* And the judgement itself, all four of its answers. */
	CHECK(!strcmp(pw3_logs_start_word(1, 7, 8), "reported"));
	CHECK(!strcmp(pw3_logs_start_word(1, 8, 8), "no_new_report"));
	CHECK(!strcmp(pw3_logs_start_word(1, 9, 8), "no_new_report"));
	CHECK(!strcmp(pw3_logs_start_word(0, 0, 8), "no_previous_report"));
	CHECK(!strcmp(pw3_logs_start_word(-1, 0, 8), "unknown"));
	/* Neither word claims the framework came up, and neither claims nothing
	 * ran: the counter settles neither (userlog.h). */
	CHECK(!strstr(pw3_logs_start_word(1, 7, 8), "started"));
	CHECK(!strstr(pw3_logs_start_word(1, 8, 8), "started"));
	return 0;
}

int main(void)
{
	char root[] = "pw3-userlog-test";
	if (test_filter())
		return 1;
	if (test_stream())
		return 1;
	if (test_report())
		return 1;
	if (test_room())
		return 1;
	if (test_rotate(root))
		return 1;
	if (test_report_max(root))
		return 1;
	printf("PW3_USERLOG_OK checks=%u\n", checks);
	return 0;
}
