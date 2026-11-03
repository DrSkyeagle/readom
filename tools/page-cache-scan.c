/* SPDX-License-Identifier: Apache-2.0 */
/* page-cache-scan ROOT...: which file pages are in the page cache. For every
 * regular file under each ROOT (symbolic links not followed, other file systems
 * not entered) it prints a line "RESIDENT_KB SIZE_KB PATH" when any page is
 * resident, and a total at the end. Two scans around a cold start, after the
 * caches are dropped, tell which files the start read (less what it evicted again,
 * so compare the sum with pgpgin in /proc/vmstat). Read-only: files are mapped
 * PROT_READ and only mincore() is asked, which reads no file data. Run as root. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <ftw.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHUNK (64L << 20)

static long page;
static unsigned char *vec;
static unsigned long long total_kb, files;

static int visit(const char *path, const struct stat *st, int type, struct FTW *ftw) {
    (void)ftw;
    if (type != FTW_F || !S_ISREG(st->st_mode) || st->st_size == 0) return 0;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOATIME);
    if (fd < 0) fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned long long resident = 0;
    for (off_t off = 0; off < st->st_size; off += CHUNK) {
        size_t len = st->st_size - off < CHUNK ? (size_t)(st->st_size - off) : (size_t)CHUNK;
        void *p = mmap(NULL, len, PROT_READ, MAP_SHARED, fd, off);
        if (p == MAP_FAILED) break;
        size_t pages = (len + page - 1) / page;
        if (mincore(p, len, vec) == 0)
            for (size_t i = 0; i < pages; i++) resident += vec[i] & 1;
        munmap(p, len);
    }
    close(fd);
    if (resident) {
        unsigned long long kb = resident * (page / 1024);
        printf("%llu %llu %s\n", kb, (unsigned long long)(st->st_size + 1023) / 1024, path);
        total_kb += kb;
        files++;
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: page-cache-scan ROOT...\n"); return 2; }
    page = sysconf(_SC_PAGESIZE);
    vec = malloc(CHUNK / page);
    if (!vec) return 1;
    for (int i = 1; i < argc; i++)
        if (nftw(argv[i], visit, 32, FTW_PHYS | FTW_MOUNT) != 0) perror(argv[i]);
    printf("TOTAL %llu KB in %llu files\n", total_kb, files);
    return 0;
}
