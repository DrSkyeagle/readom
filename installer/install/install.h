/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * The device-side installer (installer/README.md, "安装流程"): makes sure the
 * user store holds this release's system image and a data image Android can
 * use, before the entry starts Android. Every step can be started again: after
 * a power loss, a forced restart or the library stopping the script, the next
 * tap continues from what is on the user store.
 *
 *   <user>/android/system.img   this release's system image
 *   <user>/android/data.img     the user's data; made once from the template
 *   <user>/android/state        what was verified (written to a temporary file,
 *                               synced, then renamed)
 *   <user>/android/download/    the system payload while it downloads
 *
 * What touches the world outside these files (battery, network, download,
 * screen, clock) goes through inst_ops, so tests run the same steps with fakes.
 */
#ifndef PW3_INSTALL_H
#define PW3_INSTALL_H

#include <stddef.h>
#include <stdint.h>

/* How many places the payload may be fetched from. Four rather than a growing
 * list: each origin costs one attempt when it is unreachable, and a reader
 * waiting on a Kindle has a limit to what is worth trying before being told to
 * use a computer instead. */
#define INST_MAX_ORIGINS 4

/* Shown to the user as E<code> with a sentence in Chinese and English (README.md, "错误码"). */
enum inst_code {
	INST_READY = 0,
	INST_E_LOW_BATTERY = 102,	/* below the minimum and not charging */
	INST_E_RUNNING = 103,		/* another tap's installer is still working */
	INST_E_DOWNLOAD = 201,		/* the download failed; what was downloaded is kept */
	INST_E_OFFLINE = 202,		/* a download is needed and Wi-Fi is not connected */
	INST_E_CLOCK = 203,		/* the device's clock is before the release: TLS would fail */
	INST_E_PAYLOAD_BAD = 302,	/* the payload is incomplete or damaged; it was deleted */
	INST_E_RELEASE = 303,		/* the release does not match its manifest, or is missing */
	INST_E_DATA_FORMAT = 304,	/* the data image is not one this release can use */
	INST_E_NO_SPACE = 401,		/* inst_result.short_bytes more are needed */
	INST_E_IO = 402,		/* reading or writing the user store failed */
	INST_E_MEMORY = 403,
	/* a cluster the installation would write over belongs to another file as
	 * well, so nothing was written (installer/install/fatcheck.h) */
	INST_E_FAT_CROSS = 404,
};

enum inst_step {
	INST_STEP_CHECK = 1,
	INST_STEP_DOWNLOAD,		/* done/total: bytes of the payload */
	INST_STEP_VERIFY,		/* done/total: bytes of the payload read */
	INST_STEP_SYSTEM,		/* done/total: bytes of the system image on the device */
	INST_STEP_DATA,			/* done/total: bytes of the data image on the device */
	/* Android refused the system image that is on the store, so this tap writes
	 * it again. Shown once, before the installing screens, and held for a few
	 * seconds: without it the owner taps a second time after a start that did
	 * nothing visible and watches the same installation they already watched,
	 * with nothing anywhere saying why. No progress; the steps that follow carry
	 * that. */
	INST_STEP_REPAIR,
};

struct inst_image {
	uint64_t bytes;
	uint8_t sha256[32];
};

/* What the entry carries (build_payload.py's manifest, made into C by the build). */
struct inst_release {
	const char *version;		/* e.g. "v40": names the payload, kept in the state */
	int64_t built;			/* Unix time of the build: an earlier clock cannot check TLS */
	unsigned data_format;		/* data images of this format are kept */
	/* The data image format this release grows in place, and how large that
	 * image was (build_payload.py's manifest "data_upgrade"). A kept image of
	 * exactly that format and size is not refused: the file is extended to
	 * `data.bytes` on the user store and the state says this release's format,
	 * and the entry grows the filesystem inside it before Android mounts it
	 * (preinit V13, "data image grown in place"). Zero when this format has no
	 * predecessor -- the first release of a format. */
	unsigned data_legacy_format;
	uint64_t data_legacy_bytes;
	const char *payload_name;	/* readom-system-<version>.img.zst */
	/* Where the payload can be fetched, in the order to try. More than one
	 * because reachability is not a property of the release: on 2026-10-31 the
	 * one address this carried (readom.app, on a Cloudflare plan without a
	 * mainland presence) gave this project's own device 515 KB of 191 MB in 26
	 * minutes and then stopped answering, while the same bytes came back at
	 * 390 KB/s from another network. One address makes every reader hostage to
	 * one provider's route to them, and no amount of retrying is downstream of
	 * that.
	 *
	 * Every origin must serve the same bytes. Nothing here assumes they do: the
	 * payload is admitted by `payload.sha256` after the whole of it has arrived,
	 * so an origin holding a different file of the same length is refused by the
	 * hash, and one holding a shorter file is refused by the length (and takes
	 * only itself out of the running, not the release). */
	const char *payload_urls[INST_MAX_ORIGINS];
	int payload_origins;		/* how many of payload_urls are set, 1 or more */
	struct inst_image payload;
	struct inst_image system;	/* what the payload unpacks to */
	const unsigned char *template_data;	/* the empty data image, zstd */
	struct inst_image template_file;
	struct inst_image data;		/* what the template unpacks to */
	/* The system's ART boot image (export-aosp-images.py hashes
	 * /system/framework/arm/boot*). Installing a system whose boot image differs
	 * from the one in place makes Android compile every application in /data
	 * again on the first boot, before the home screen; installing one with the same boot
	 * image does not. This is what tells the two apart. */
	uint8_t boot_image[32];
	/* ro.build.version.incremental of the system image above, read out of that
	 * image at build time (build_launcher.py, with the image's sha256 checked
	 * against the payload's first). Every report the Android side writes
	 * carries this property, so a report can be told to be from the system this
	 * entry installs rather than from whatever was installed before it -- which
	 * is the only question the compiling notice asks. Never empty: the entry is
	 * not built without it. */
	const char *build_incremental;
	const unsigned char *ca_bundle;	/* PEM, for the download */
	size_t ca_bundle_bytes;
};

struct inst_ops {
	void *ctx;
	/* 0 and the level in percent, or -1 when it cannot be read (the check is skipped). */
	int (*battery)(void *ctx, int *percent, int *charging);
	int (*online)(void *ctx);
	int64_t (*now)(void *ctx);
	/* Downloads url to the end of part (resuming what is there; it may be absent),
	 * trusting only ca_file; returns curl's exit status (0: complete) and sets *http
	 * to the last HTTP status (0 if none). progress() may be called with the part's
	 * size while it runs. A part past limit bytes ends the download with 63 (curl's
	 * CURLE_FILESIZE_EXCEEDED), whatever the server answers -- the implementation
	 * watches the file rather than trusting the answer's stated length, so a reply
	 * that states none does not get past it. It is a limit the part is *found*
	 * past, not one it cannot reach: the watching is periodic, so between two looks
	 * the file can grow (kindle.c bounds how far with RLIMIT_FSIZE). Without it a
	 * wrong or hostile answer could fill the store the user keeps books in, and the
	 * size is only known once the whole answer has been written. */
	int (*download)(void *ctx, const char *url, const char *part, const char *ca_file, uint64_t limit,
			void (*progress)(void *pctx, uint64_t bytes), void *pctx, int *http);
	void (*wait)(void *ctx, unsigned seconds);
	void (*show)(void *ctx, enum inst_step step, uint64_t done, uint64_t total);
	void (*log)(void *ctx, const char *line);
};

struct inst_paths {
	const char *user;	/* the user store: "/mnt/us" */
	const char *work;	/* an existing private directory for the CA bundle and template */
};

struct inst_result {
	enum inst_code code;
	/* INST_READY: the system image was replaced and the user's data image kept. The
	 * applications in it are compiled again on the first boot (frameworks/base 0026). */
	int upgraded;
	/* INST_READY: a report said Android had refused the system image on the
	 * store, so it was written again (docs/known-issues.md, 七). For the log and
	 * for the tests; the owner was told on the screen while it happened. */
	int repaired;
	uint64_t short_bytes;	/* INST_E_NO_SPACE */
	int sys_errno;
	char detail[160];	/* a short English reason for the log */
};

/* Battery level below which an install does not start unless charging. */
#define INST_MIN_BATTERY 40
/* Left free on the user store for the stock system once everything is written. */
#define INST_RESERVE_BYTES (64ull << 20)

enum inst_code inst_ensure(const struct inst_release *release, const struct inst_paths *paths,
			   const struct inst_ops *ops, struct inst_result *result);

#endif
