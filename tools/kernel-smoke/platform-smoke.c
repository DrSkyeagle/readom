/* SPDX-License-Identifier: MIT */
/* run only inside the diskless test initramfs. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include "binder.h"
#include "ashmem.h"

static int binder(void) {
    const char *paths[] = {"/dev/binder", "/dev/hwbinder", "/dev/vndbinder"};
    int fds[3] = {-1,-1,-1}, failed = 0;
    for (int i=0; i<3; ++i) {
        struct binder_version version = {0};
        fds[i] = open(paths[i], O_RDWR|O_CLOEXEC);
        if (fds[i] < 0 || ioctl(fds[i], BINDER_VERSION, &version) ||
            version.protocol_version != 7 || ioctl(fds[i], BINDER_SET_CONTEXT_MGR, 0)) {
            fprintf(stderr,"FAIL Binder %s version=%d errno=%d\n",paths[i],version.protocol_version,errno);
            failed=1; break;
        }
    }
    for (int i=0; i<3; ++i) if (fds[i]>=0) close(fds[i]);
    if (!failed) puts("PASS Binder 32-bit protocol and three independent context managers");
    return failed;
}
static int ashmem(void) {
    int failed=1,fd=open("/dev/ashmem",O_RDWR|O_CLOEXEC);
    char name[256]="pw3-test", received[256]={0};
    void *p=MAP_FAILED,*forbidden=MAP_FAILED;
    if (fd<0 || ioctl(fd,ASHMEM_SET_NAME,name) || ioctl(fd,ASHMEM_SET_SIZE,4096) ||
        ioctl(fd,ASHMEM_GET_NAME,received) || strcmp(received,name) ||
        ioctl(fd,ASHMEM_GET_SIZE,0)!=4096) goto done;
    p=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (p==MAP_FAILED) goto done;
    memcpy(p,"ARM-ashmem",11);
    if (memcmp(p,"ARM-ashmem",11) || ioctl(fd,ASHMEM_SET_PROT_MASK,PROT_READ)) goto done;
    errno=0;
    forbidden=mmap(NULL,4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if (forbidden!=MAP_FAILED || errno!=EPERM) goto done;
    failed=0;
done:
    if (failed) fprintf(stderr,"FAIL ashmem errno=%d\n",errno);
    else puts("PASS ashmem size/name/shared pixels and write-protection downgrade");
    if (p!=MAP_FAILED) munmap(p,4096);
    if (forbidden!=MAP_FAILED) munmap(forbidden,4096);
    if (fd>=0) close(fd);
    return failed;
}
static int wakeup(void) {
    int fd=open("/sys/power/wake_lock",O_WRONLY|O_CLOEXEC);
    const char *name="pw3_kernel_smoke";
    size_t length=strlen(name);
    if (fd<0 || write(fd,name,length)!=(ssize_t)length) { if(fd>=0)close(fd); perror("wake_lock"); return 1; }
    close(fd);
    fd=open("/sys/power/wake_unlock",O_WRONLY|O_CLOEXEC);
    if (fd<0 || write(fd,name,length)!=(ssize_t)length) { if(fd>=0)close(fd); perror("wake_unlock"); return 1; }
    close(fd); puts("PASS userspace wake_lock/wake_unlock pair"); return 0;
}
static int mount_namespace(void) {
    pid_t child=fork(); int status;
    if (child<0) return 1;
    if (!child) {
        if (unshare(CLONE_NEWNS)) { perror("Zygote unshare"); _exit(1); }
        chmod("/tmp",0755); mkdir("/tmp/proc",0755);
        if (mount("proc","/tmp/proc","proc",0,"hidepid=2,gid=3009")) { perror("Android proc hidepid mount"); _exit(2); }
        if (setgid(10000) || setuid(10000)) _exit(3);
        struct stat own,other;
        char path[80]; snprintf(path,sizeof(path),"/tmp/proc/%ld",(long)getpid());
        if (stat(path,&own)) { perror("own proc entry"); _exit(4); }
        errno=0;
        if (stat("/tmp/proc/1",&other)==0 || (errno!=ENOENT && errno!=EPERM && errno!=EACCES)) {
            fputs("FAIL hidepid exposes root PID to app UID\n",stderr); _exit(5);
        }
        _exit(0);
    }
    if (waitpid(child,&status,0)!=child || !WIFEXITED(status) || WEXITSTATUS(status)) return 1;
    puts("PASS mount namespace and proc hidepid cross-UID isolation"); return 0;
}
int main(void) {
    setvbuf(stdout,NULL,_IONBF,0);
    if (getppid()!=1) { fputs("Use the diskless test initramfs.\n",stderr); return 2; }
    return binder()+ashmem()+wakeup()+mount_namespace() ? 1 : 0;
}
