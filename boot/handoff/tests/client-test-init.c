/* SPDX-License-Identifier: GPL-2.0-only */
/* Dedicated disposable VM only. Checks actual helper/stock-shell cleanup on FAT. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned checks;
static void finish(int good) { printf("PW3_FIRST_KERNEL_CLIENT_TEST_%s checks=%u\n",good?"PASS":"FAIL",checks);fflush(stdout);sync();reboot(RB_POWER_OFF);for(;;)pause(); }
#define CHECK(x) do {checks++;if(!(x)){printf("FAIL line=%u errno=%d\n",__LINE__,errno);finish(0);}} while(0)
static void write_text(const char *path,const char *text) {
 int fd=open(path,O_WRONLY|O_CREAT|O_TRUNC,0600);CHECK(fd>=0);size_t n=strlen(text);CHECK(write(fd,text,n)==(ssize_t)n);CHECK(close(fd)==0);
}
static void read_text(const char *path,char *text,size_t capacity) {
 int fd=open(path,O_RDONLY);CHECK(fd>=0);ssize_t n=read(fd,text,capacity-1);CHECK(n>=0&&(size_t)n<capacity-1);text[n]=0;CHECK(close(fd)==0);
}
static void copy_file(const char *from,const char *to) {
 int src=open(from,O_RDONLY),dst=open(to,O_WRONLY|O_CREAT|O_EXCL,0600);CHECK(src>=0&&dst>=0);char b[65536];ssize_t n;
 while((n=read(src,b,sizeof(b)))>0) {size_t off=0;while(off<(size_t)n){ssize_t count=write(dst,b+off,(size_t)n-off);CHECK(count>0);off+=(size_t)count;}}
 CHECK(n==0&&fsync(dst)==0);CHECK(close(src)==0&&close(dst)==0);
}
static void run_case(unsigned number,const char *fault,int wrapper) {
 char out[128],path[200],data[8192];snprintf(out,sizeof(out),"/mnt/us/pw3-first-kernel-%u",number);
 if(fault)write_text("/test-fault-stage",fault);else if(unlink("/test-fault-stage"))CHECK(errno==ENOENT);
 if(!wrapper)CHECK(mkdir(out,0700)==0);
 pid_t child=fork();CHECK(child>=0);
 if(!child) {
  if(wrapper)execl("/bin/sh","/bin/sh","/mnt/us/documents/PW3-First-Kernel.sh",out,(char*)NULL);
  else {
   snprintf(path,sizeof(path),"%s/helper.log",out);int log=open(path,O_WRONLY|O_CREAT|O_EXCL,0600);
   if(log<0||dup2(log,1)<0||dup2(log,2)<0) _exit(126);
   close(log);
   execl("/helper","/helper",out,(char*)NULL);
  }
  _exit(127);
 }
 int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status));
 snprintf(path,sizeof(path),"%s/%s",out,wrapper?"launcher.log":"helper.log");read_text(path,data,sizeof(data));
 printf("CASE %u fault=%s wrapper=%d exit=%d\n%s\n",number,fault?fault:"none",wrapper,WEXITSTATUS(status),data);
 CHECK(WEXITSTATUS(status)==1);
 CHECK(access("/sys/module/pw3_first_kernel",F_OK)<0&&errno==ENOENT);
 char workspace[160];snprintf(workspace,sizeof(workspace),"/tmp/pw3-first-kernel-%ld",(long)child);CHECK(access(workspace,F_OK)<0&&errno==ENOENT);
 snprintf(path,sizeof(path),"%s/result.txt",out);
 CHECK(strstr(data,"check=pm_test_sysfs_available")!=NULL);
 CHECK(access(path,F_OK)<0&&errno==ENOENT);
 if(wrapper) {
  snprintf(workspace,sizeof(workspace),"/tmp/pw3-first-kernel-launch-%ld",(long)child);CHECK(access(workspace,F_OK)<0&&errno==ENOENT);
  snprintf(path,sizeof(path),"%s/launcher-complete.txt",out);read_text(path,data,sizeof(data));CHECK(strstr(data,"helper_exit=1\n"));
  child=fork();CHECK(child>=0);if(!child){CHECK(chdir(out)==0);execl("/bin/busybox","busybox","sha256sum","-c","SHA256SUMS",(char*)NULL);_exit(127);}CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
 }
}
int main(void) {
 CHECK(getpid()==1);setvbuf(stdout,NULL,_IOLBF,0);
 CHECK(mkdir("/proc",0555)==0&&mkdir("/sys",0555)==0&&mkdir("/tmp",0700)==0&&mkdir("/mnt",0755)==0&&mkdir("/mnt/us",0700)==0);
 CHECK(mount("proc","/proc","proc",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)==0);
 CHECK(mount("sysfs","/sys","sysfs",MS_NOSUID|MS_NODEV|MS_NOEXEC,NULL)==0);
 CHECK(mount("devtmpfs","/dev","devtmpfs",MS_NOSUID,"mode=0755")==0);
 for(int i=0;i<100&&access("/dev/mmcblk0",F_OK);i++)usleep(20000);
 CHECK(access("/dev/mmcblk0",F_OK)==0);
 CHECK(mount("/dev/mmcblk0","/mnt/us","vfat",MS_NOSUID|MS_NODEV|MS_NOEXEC,"umask=0077")==0);
 CHECK(mkdir("/mnt/us/documents",0700)==0);copy_file("/launcher.sh","/mnt/us/documents/PW3-First-Kernel.sh");
 run_case(10,NULL,1);CHECK(umount("/mnt/us")==0);finish(1);return 1;
}
