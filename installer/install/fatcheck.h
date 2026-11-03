/* SPDX-License-Identifier: Apache-2.0
 * Whether a cluster the installer is about to write over belongs to another
 * file as well.
 *
 * Where this asks is not where the installer writes. On the device the user
 * store the launcher works in is /mnt/us, which is fsp -- Amazon's FUSE proxy
 * -- over the real vfat at /mnt/base-us, itself a loop device over mmcblk0p4 at
 * a sector offset (boot/devicedata/devicedata.c, storage_lines, which refuses
 * to build a boot configuration unless the store is exactly that). A FUSE mount
 * has no block device of its own and answers no FIBMAP, so asked there the
 * question has no answer at all. The filesystem that will hold the bytes is the
 * vfat underneath, and that is the one this asks.
 *
 * The images are the same files under both mounts -- fsp proxies the one tree,
 * which is not read off here but out of working code: every boot of the launcher
 * chain opens /mnt/base-us/android/data.img, reads its ext4 superblock and takes
 * the filesystem UUID the start is told to admit from it (devicedata.c,
 * data_conf), and it has never been handed anything but the image the installer
 * wrote through /mnt/us. So the
 * path to ask about is the image's path with the store's prefix replaced by the
 * vfat's mount point. Where the store is itself the vfat, which is every case
 * without a proxy, that substitution is the path itself and nothing here needs
 * to know the difference.
 *
 * Which vfat, then, is settled by the file and not by a count of mounts: the
 * volume is the mounted vfat that holds that path, at exactly the length this
 * release writes, on its own filesystem. A machine with a second FAT mounted
 * for its own reasons -- an EFI partition -- is thereby not a machine where the
 * question goes unasked.
 *
 * The loop device is also why no offset is passed to the shared check: the
 * offset is the loop device's own, so sector 0 of what the vfat is mounted from
 * is the volume's first sector -- the frame the boot sector's numbers and
 * FIBMAP's answers are both in.
 *
 * The check itself is shared with the start that maps these images
 * (device/amazon/pw3/preinit/FatCrossLink.h): both passes, the table and the
 * directory, and the proof that each file is named exactly once. What is not
 * shared is how each program learns where its own file lies, because the two ask
 * under different conditions and in different frames. preinit asks of a FAT it
 * has mounted read-only and wants the answers in the partition's frame, because
 * its device-mapper targets are built on the partition. This asks of the vfat
 * the stock system has mounted read-write -- it is itself the writer, by way of
 * the proxy -- and wants them in the volume's own frame.
 *
 * So the receipt's shape (FileMapFormat.h) and the algorithm (FatCrossLink.h)
 * are shared, and nothing else is. Making one mapping reader serve both would
 * mean a parameter for the frame and a second for whether a read-only mount is
 * required, inside a header the first process runs; two callers of the same
 * kernel interface under different rules are not a contract, and the installer
 * writes no receipt for anyone to read back.
 */
#ifndef PW3_INSTALL_FATCHECK_H
#define PW3_INSTALL_FATCHECK_H
#include <stdint.h>

enum inst_fat {
	INST_FAT_OK,		/* no cluster of these files belongs to anything else */
	INST_FAT_SHARED,	/* one does: writing here would write into that file */
	INST_FAT_UNREADABLE	/* the volume could not be found, read or understood */
};

/* `where` is a literal naming the step that did not finish, for the log; `err`
 * is errno when a call failed and zero when the answer itself was a refusal.
 * `detail` is the shared check's own code, so a refusal says which pass found
 * it.
 *
 * `mount` and `volume` are where it looked -- the vfat's mount point and the
 * device it is mounted from -- and stay empty until both are known. An install
 * on a reader's Kindle is the first run of this code on a store shaped like
 * theirs, so the line it leaves behind has to say whether the question was
 * asked at all, and of what. */
struct inst_fat_note {
	const char *where;
	int err;
	int detail;
	char mount[96];
	char volume[96];
};

/* The images are named by their paths in the user store, with the length each
 * one has now -- not the length this release will give it. A mapping is only a
 * fact about the clusters a file holds at the moment it is taken, and those are
 * the clusters the write goes through: the unpack opens the image as it is and
 * writes it from the first byte, so an image left shorter by an earlier install
 * or by a repair tool is overwritten through its own chain, whatever its length.
 * Asking about the length it is going to have would skip exactly that file. A
 * path may be NULL, which skips it. */
enum inst_fat inst_fat_exclusive(const char *user_dir,
				 const char *system_path, uint64_t system_bytes,
				 const char *data_path, uint64_t data_bytes,
				 struct inst_fat_note *note);

#endif
