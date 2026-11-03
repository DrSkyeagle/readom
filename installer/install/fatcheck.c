// SPDX-License-Identifier: Apache-2.0
#define _GNU_SOURCE
#include "fatcheck.h"
#include "FatCrossLink.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <unistd.h>

#define SECTOR 512u

/* FIBMAP and FIGETBSZ live in <linux/fs.h>, and this program is the one user of
 * them that cannot have that header: the installer is linked static against musl
 * (boot/build_launcher.py), which ships no kernel headers, while the start that
 * makes the same two calls is built by AOSP, whose bionic carries the whole UAPI
 * tree. Their numbers are all that is needed and the ABI fixes them --
 * `#define FIBMAP _IO(0x00,1)` and `#define FIGETBSZ _IO(0x00,2)`, which are 1
 * and 2, in this device's kernel (3.0.35) as in every kernel since. Writing a
 * number by hand is how this project reaches an interface its toolchain does not
 * declare (boot/handoff-init/pw3-handoff-init.c does it for the EPDC's), and
 * where the header is there it is included and the numbers are checked against
 * it rather than trusted: the host tests build this file against glibc. */
#if defined(__has_include) && __has_include(<linux/fs.h>)
#include <linux/fs.h>
_Static_assert(FIBMAP == 1 && FIGETBSZ == 2,
	       "FIBMAP/FIGETBSZ are not the numbers this file uses where the header is absent");
#else
#define FIBMAP 1
#define FIGETBSZ 2
#endif

static enum inst_fat refuse(struct inst_fat_note *note, const char *where, int err)
{
	note->where = where;
	note->err = err;
	return INST_FAT_UNREADABLE;
}

/* One field of a /proc/mounts line, with the four escapes the kernel writes for
 * it (mangle_path: space, tab, newline and backslash itself). Without this a
 * mount point holding a space would be stat'ed under a name that is not there,
 * the volume would not be found, and the install would go on unchecked -- a safe
 * answer, but the wrong one, and the kind that is never noticed because nothing
 * about it looks like a failure. */
static int field(const char **at, char *out, size_t cap)
{
	const char *p = *at;
	size_t n = 0;

	while (*p == ' ')
		p++;
	if (!*p || *p == '\n')
		return 0;
	for (; *p && *p != ' ' && *p != '\n'; p++) {
		unsigned char c = (unsigned char)*p;
		if (c == '\\' && p[1] >= '0' && p[1] <= '3' &&
		    p[2] >= '0' && p[2] <= '7' && p[3] >= '0' && p[3] <= '7') {
			c = (unsigned char)(((p[1] - '0') << 6) | ((p[2] - '0') << 3) | (p[3] - '0'));
			p += 3;
		}
		if (n + 1 >= cap)
			return 0;
		out[n++] = (char)c;
	}
	out[n] = '\0';
	*at = p;
	return 1;
}

/* The vfat the images are really on, settled by the two things that settle it:
 * the device number, and the file.
 *
 * Every vfat line of /proc/mounts is a candidate, and a candidate has to pass
 * both. The device number first: the source is stat'ed and only a block device
 * whose st_rdev is the st_dev of its own mount point can be the volume. That is
 * the name being checked against the kernel rather than believed -- a name that
 * has since been given to another device, or one that never backed this mount,
 * cannot pass; a symbolic link that leads to the right device can, which is the
 * right answer, since what is opened is the device the mount is on. Then the
 * file: under that mount point, at the image's own path, there has to be a plain
 * file of exactly the length this image has now, on that same filesystem.
 *
 * Tying the choice to the file rather than to a count of mounts is what makes
 * this work on a system that has another FAT mounted for its own reasons -- the
 * build machine has an EFI partition, and nothing says a reader's Kindle will
 * not one day have a second one. A count would make either of those "the
 * question was not asked"; the file makes them irrelevant. Two vfat volumes
 * that each hold that path at that length would be ambiguous, and the honest
 * answer to an ambiguity is still that the question was not asked.
 *
 * Candidates are counted by the file they name and not by mount point, because
 * the same volume mounted twice -- a second mount of the device, a bind mount
 * of it -- is two lines of /proc/mounts and one answer. Counting lines would
 * make that configuration an ambiguity it is not; counting the filesystem alone
 * would go too far the other way, because a bind mount of a *subdirectory* of
 * this volume has the same device number while the path under it names a
 * different file, and two different files are exactly the ambiguity this has to
 * refuse. So it is the device and the inode together. */
static int vfat_volume(const char *rel, uint64_t bytes, struct inst_fat_note *note,
		       char *mount, size_t mount_cap, dev_t *holder, ino_t *named)
{
	char source[PATH_MAX], target[PATH_MAX], type[64], line[PATH_MAX * 2], image[PATH_MAX];
	FILE *mounts;
	int fd = -1, found = 0;

	if (!(mounts = fopen("/proc/mounts", "re")))
		return refuse(note, "open /proc/mounts", errno), -1;
	while (fgets(line, sizeof(line), mounts)) {
		const char *at = line;
		struct stat block, dir, file;

		/* A line longer than this buffer would otherwise come back as a second,
		 * headless piece and be parsed as a mount of its own; the rest of it is
		 * dropped instead. Either way that candidate is missed, so "only one
		 * volume holds this image" would stop being a complete count -- which
		 * is why this skips rather than guesses. No mount on this device comes
		 * near 8 KB of names and options. */
		if (!strchr(line, '\n') && !feof(mounts)) {
			int c;
			while ((c = fgetc(mounts)) != '\n' && c != EOF) {
			}
			continue;
		}
		if (!field(&at, source, sizeof(source)) || !field(&at, target, sizeof(target)) ||
		    !field(&at, type, sizeof(type)))
			continue;
		if (strcmp(type, "vfat") || source[0] != '/' || target[0] != '/')
			continue;
		if (stat(source, &block) || !S_ISBLK(block.st_mode))
			continue;
		if (stat(target, &dir) || !S_ISDIR(dir.st_mode) || dir.st_dev != block.st_rdev)
			continue;
		if ((size_t)snprintf(image, sizeof(image), "%s%s", target, rel) >= sizeof(image))
			continue;
		if (stat(image, &file) || !S_ISREG(file.st_mode) ||
		    (uint64_t)file.st_size != bytes || file.st_dev != dir.st_dev)
			continue;
		if (found && dir.st_dev == *holder && file.st_ino == *named)
			continue;	/* the same file, reached by another mount of it */
		if (++found > 1)
			break;
		/* Every one of the three, because the log's two fields are shorter
		 * than a path: a name that does not fit is a check that did not run,
		 * which costs this install nothing, where a name silently cut short
		 * is a log line naming a volume that does not exist. */
		if ((size_t)snprintf(mount, mount_cap, "%s", target) >= mount_cap ||
		    (size_t)snprintf(note->mount, sizeof(note->mount), "%s", target) >=
			    sizeof(note->mount) ||
		    (size_t)snprintf(note->volume, sizeof(note->volume), "%s", source) >=
			    sizeof(note->volume)) {
			refuse(note, "the vfat's name does not fit the log", 0);
			found = 0;
			break;
		}
		*holder = dir.st_dev;
		*named = file.st_ino;
		if ((fd = open(source, O_RDONLY | O_CLOEXEC)) < 0) {
			refuse(note, "open the volume", errno);
			found = 0;
			break;
		}
	}
	fclose(mounts);
	if (found == 1)
		return fd;
	if (fd >= 0)
		close(fd);
	note->mount[0] = note->volume[0] = '\0';
	*holder = 0;
	*named = 0;
	if (!note->where)
		refuse(note, found ? "more than one mounted vfat holds this image"
				   : "no mounted vfat holds this image", 0);
	return -1;
}

/* The image's path within the store, which is also its path within the vfat:
 * the installer writes through the store, and the same file on the vfat is that
 * path with the store's prefix replaced by the vfat's mount point. Where the
 * store is itself the vfat, the substitution gives back the path it was handed,
 * so there is no second case here. */
static const char *relative(const char *user_dir, const char *path, struct inst_fat_note *note)
{
	size_t dir = strlen(user_dir);

	while (dir && user_dir[dir - 1] == '/')
		dir--;
	if (!dir || strncmp(path, user_dir, dir) || path[dir] != '/') {
		refuse(note, "the image is not under the user store", 0);
		return NULL;
	}
	return path + dir;
}

/* Where this file's clusters are, in the volume's own frame: FIBMAP answers in
 * units of the filesystem's block size, which vfat sets to the sector, counted
 * from the volume's first sector -- the frame the boot sector's own numbers are
 * in, so nothing is added here.
 *
 * Both ends of every cluster are asked for and a cluster whose ends do not span
 * it is refused, which is what makes the answer a cluster map rather than a
 * sector map; FAT allocates whole clusters and their sectors are contiguous, so
 * a cluster that disagrees means the answer cannot be trusted. Asking per
 * cluster rather than per sector is also what keeps this to seconds: 512 MiB at
 * 8 KiB clusters is 65,536 pairs, not 1 M single sectors.
 *
 * A hole answers zero. The images have none -- FAT cannot represent one, and
 * they are written whole -- and a zero is refused rather than skipped, because a
 * file with a hole is not a file whose clusters are known.
 *
 * A length that is not whole sectors is mapped all the same, rounded up: the
 * sector holding the last byte is allocated to this file like any other, and
 * unpack writes over it. Refusing such a length used to skip the check, which is
 * the one outcome this file exists to avoid -- a truncated image is exactly the
 * case the length was made observable for (Codex 2026-10-31).
 *
 * The length is checked here a second time, against the file on the vfat rather
 * than the one the proxy shows: two views of one file that disagree about its
 * length are not a volume this can reason about, and that is a refusal to
 * conclude, not a cross-link. */
static enum inst_fat map_file(const char *path, uint64_t bytes, dev_t holder,
			      uint64_t volume_sectors, uint64_t data_start,
			      struct pw3_file_mapping *m, struct inst_fat_note *note)
{
	struct stat file;
	struct statfs fs;
	int block_bytes = 0;
	int64_t cluster_bytes;
	uint64_t sectors;
	int cluster, total, logical, fd;
	enum inst_fat bad = INST_FAT_OK;

	memset(m, 0, sizeof(*m));
	if (!bytes)
		return refuse(note, "the image has no length", 0);
	sectors = (bytes + SECTOR - 1) / SECTOR;
	if (sectors > INT_MAX)
		return refuse(note, "the image is longer than FIBMAP can address", 0);
	if ((fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC)) < 0)
		return refuse(note, "open the image on the vfat", errno);
	if (fstat(fd, &file))
		bad = refuse(note, "fstat the image on the vfat", errno);
	else if (!S_ISREG(file.st_mode) || file.st_nlink != 1 || (uint64_t)file.st_size != bytes ||
		 file.st_dev != holder || !file.st_ino)
		bad = refuse(note, "the image on the vfat is not one plain file of that length", 0);
	else if (fstatfs(fd, &fs))
		bad = refuse(note, "fstatfs the image", errno);
	else if ((uint32_t)fs.f_type != 0x4d44)
		bad = refuse(note, "the image is not on a FAT", 0);
	else if (ioctl(fd, FIGETBSZ, &block_bytes))
		bad = refuse(note, "FIGETBSZ", errno);
	else if (block_bytes != (int)SECTOR)
		bad = refuse(note, "the filesystem's block is not one sector", 0);
	if (bad != INST_FAT_OK) {
		close(fd);
		return bad;
	}

	/* FAT reports its cluster size as the block size of statfs. */
	cluster_bytes = (int64_t)fs.f_bsize;
	if (cluster_bytes < SECTOR || cluster_bytes > 65536 || (cluster_bytes & (cluster_bytes - 1))) {
		close(fd);
		return refuse(note, "the cluster size is not a power of two in range", 0);
	}
	cluster = (int)(cluster_bytes / SECTOR);
	total = (int)sectors;
	m->sectors = sectors;
	m->device = file.st_dev;
	m->inode = file.st_ino;
	for (logical = 0; logical < total; logical += cluster) {
		/* The last cluster of a file may reach past its end. */
		const int span = total - logical < cluster ? total - logical : cluster;
		int first = logical, last = logical + span - 1;
		uint64_t start;
		struct pw3_file_extent *tail;

		if (ioctl(fd, FIBMAP, &first) || ioctl(fd, FIBMAP, &last)) {
			bad = refuse(note, "FIBMAP", errno);
			break;
		}
		if (first <= 0 || last <= 0) {
			bad = refuse(note, "the image has a hole", 0);
			break;
		}
		if ((uint64_t)first < data_start || (uint64_t)first >= volume_sectors ||
		    (uint64_t)last >= volume_sectors) {
			bad = refuse(note, "a cluster lies outside the volume's data area", 0);
			break;
		}
		if (last != first + span - 1) {
			bad = refuse(note, "a cluster's sectors are not contiguous", 0);
			break;
		}
		start = (uint64_t)first;
		tail = m->count ? &m->extents[m->count - 1] : NULL;
		if (tail && tail->start + tail->sectors == start) {
			tail->sectors += (uint64_t)span;
			continue;
		}
		if (m->count == PW3_FILEMAP_MAX_EXTENTS) {
			bad = refuse(note, "the image is in more than 1024 pieces", 0);
			break;
		}
		tail = &m->extents[m->count++];
		tail->logical = (uint64_t)logical;
		tail->start = start;
		tail->sectors = (uint64_t)span;
	}
	close(fd);
	return bad;
}

/* One pass: where both images lie, and what the shared check says about those
 * clusters. Everything a pass reads it reads afresh -- the boot sector, the
 * table, the directory tree -- because the point of asking twice is to ask of
 * the volume as it is now.
 *
 * Read-write is the condition here, not a hazard to rule out: the installer is
 * the writer. What it has to be sure of is that the clusters it will write are
 * nobody else's, and FAT does not move an allocated cluster of a file that is
 * not being extended -- so a map taken now describes the same clusters the
 * write will reach, as long as the write stays inside the length that was
 * mapped. That is why the lengths are the lengths these files have now. */
static enum inst_fat one_pass(int fd, const char *mount, dev_t holder,
			      const char *rel_system, uint64_t system_bytes,
			      const char *rel_data, uint64_t data_bytes,
			      struct inst_fat_note *note)
{
	unsigned char boot[SECTOR];
	char path[PATH_MAX];
	struct pw3_fat_geometry g;
	struct pw3_file_maps maps;
	enum pw3_fat_cross why;
	uint64_t volume_sectors;
	enum inst_fat bad;
	off_t end;

	memset(&maps, 0, sizeof(maps));
	maps.magic = PW3_FILEMAP_MAGIC;

	/* The descriptor has to be one on the filesystem itself -- syncfs flushes
	 * the filesystem its descriptor belongs to, and the volume's own descriptor
	 * belongs to the one /dev is on. */
	{
		const int on_fat = open(mount, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		const int flushed = on_fat >= 0 && !syncfs(on_fat);
		const int why_not = errno;

		if (on_fat >= 0)
			close(on_fat);
		if (!flushed)
			return refuse(note, "flush the volume", why_not);
	}

	if ((end = lseek(fd, 0, SEEK_END)) <= 0)
		return refuse(note, "the volume's size", errno);
	volume_sectors = (uint64_t)end / SECTOR;
	if (pread(fd, boot, sizeof(boot), 0) != (ssize_t)sizeof(boot))
		return refuse(note, "read the boot sector", errno);
	/* The device is mounted as FAT, so its first sector is a boot sector; this
	 * says the numbers in it also describe a volume that fits on the device,
	 * before any of them is used as an offset. pw3_fat_bounds counts bytes --
	 * it compares total * sector against what it is given -- while everything
	 * below counts 512-byte sectors, which is the frame FIBMAP and
	 * pw3_fat_data_start both answer in. */
	if (!pw3_fat_bounds(boot, (uint64_t)end) || !pw3_fat_geometry(boot, &g))
		return refuse(note, "the boot sector does not describe this volume", 0);

	if (rel_system) {
		if ((size_t)snprintf(path, sizeof(path), "%s%s", mount, rel_system) >= sizeof(path))
			return refuse(note, "the image's name on the vfat is too long", 0);
		bad = map_file(path, system_bytes, holder, volume_sectors, g.data_start,
			       &maps.map[PW3_FILE_SYSTEM], note);
		if (bad != INST_FAT_OK)
			return bad;
	}
	if (rel_data) {
		if ((size_t)snprintf(path, sizeof(path), "%s%s", mount, rel_data) >= sizeof(path))
			return refuse(note, "the image's name on the vfat is too long", 0);
		bad = map_file(path, data_bytes, holder, volume_sectors, g.data_start,
			       &maps.map[PW3_FILE_DATA], note);
		if (bad != INST_FAT_OK)
			return bad;
	}
	if (!maps.map[PW3_FILE_SYSTEM].count && !maps.map[PW3_FILE_DATA].count)
		return refuse(note, "neither image has a cluster", 0);

	/* Zero, because the extents above are already in the volume's frame. */
	why = pw3_fat_cross_check(fd, boot, 0, &maps);
	note->detail = (int)why;
	if (why == PW3_FAT_CROSS_OK)
		return INST_FAT_OK;
	/* Only one of the shared check's answers says a cluster of these images
	 * belongs to something else as well. The rest are what it could not do:
	 * PW3_FAT_CROSS_DIRECTORY, in particular, is a directory tree deeper than
	 * 32 levels or one that met the same cluster twice -- and the walk covers
	 * the whole volume, so a reader with a legitimate 33-level folder of their
	 * own, nowhere near these images, would have every install refused. "Could
	 * not finish looking" is not "looked and found sharing"; preinit refuses on
	 * it because it is about to build a device-mapper target on that map, and
	 * nothing here is. */
	if (why == PW3_FAT_CROSS_SHARED) {
		note->where = pw3_fat_cross_text(why);
		note->err = 0;
		return INST_FAT_SHARED;
	}
	return refuse(note, pw3_fat_cross_text(why), 0);
}

/* The boundary, stated rather than left to be discovered: this answers for a
 * file that is already there, about the clusters it has now -- which is what the
 * write goes through, because the unpack opens the image and writes it from the
 * first byte (installer/unpack/unpack.c). A file the install creates has its
 * clusters chosen by the filesystem out of the free map, and a free map that
 * offers a cluster another file is using is a different fault from a chain that
 * is shared -- one this cannot see before the write, because the clusters do not
 * exist to be asked about until the write allocates them. The same goes for the
 * part of a longer image that is beyond what is there now. */
enum inst_fat inst_fat_exclusive(const char *user_dir,
				 const char *system_path, uint64_t system_bytes,
				 const char *data_path, uint64_t data_bytes,
				 struct inst_fat_note *note)
{
	char mount[PATH_MAX];
	const char *rel_system = NULL, *rel_data = NULL;
	enum inst_fat bad = INST_FAT_UNREADABLE;
	dev_t holder = 0;
	ino_t named = 0;
	int fd, attempt;

	memset(note, 0, sizeof(*note));

	/* The paths within the store first, because the volume is found by one of
	 * them: which filesystem to ask is settled by where the file is. */
	if (system_path && !(rel_system = relative(user_dir, system_path, note)))
		return INST_FAT_UNREADABLE;
	if (data_path && !(rel_data = relative(user_dir, data_path, note)))
		return INST_FAT_UNREADABLE;
	if (!rel_system && !rel_data)
		return refuse(note, "nothing was given to check", 0);

	fd = vfat_volume(rel_system ? rel_system : rel_data,
			 rel_system ? system_bytes : data_bytes,
			 note, mount, sizeof(mount), &holder, &named);
	if (fd < 0)
		return INST_FAT_UNREADABLE;

	/* Asked twice at the most, and only a refusal that comes back refuses.
	 *
	 * The two halves of this question are asked of two different things: FIBMAP
	 * answers from the in-memory inode, while the table and the directory tree
	 * are read through the block device, which the filesystem's own caches are
	 * not behind. They are comparable only while the volume on disk is the
	 * volume the kernel has, and on a FAT mounted read-write that is not given:
	 * the kernel may be holding the newer of the two, and the stock system goes
	 * on writing to this store while the installer runs.
	 *
	 * The check's last rule is the sharpest: each mapped image must be named by
	 * exactly one directory entry, so an image whose directory has not reached
	 * the disk, or whose entry is rewritten between the mapping and the walk,
	 * reads as a cross-link where there is none -- the one failure that would
	 * cost a reader an install that was going to work.
	 *
	 * So two things are done rather than one assumed. Each pass flushes the
	 * filesystem first, which settles everything a quiet volume can be wrong
	 * about (and is why preinit needs no such call: there the FAT is mounted
	 * read-only and nothing has written to it since the stock system let go).
	 * And a refusal has to be reproducible: a volume that really shares a
	 * cluster says so every time it is asked, where a volume that was moving
	 * under the first pass says it once. The second pass costs another read of
	 * the table and the directories -- 60 ms and a few hundred KB on this
	 * device -- against refusing an install over a write that happened to land
	 * between two reads. */
	for (attempt = 0; attempt < 2; ++attempt) {
		bad = one_pass(fd, mount, holder, rel_system, system_bytes, rel_data, data_bytes, note);
		if (bad != INST_FAT_SHARED)
			break;
	}
	close(fd);
	return bad;
}
