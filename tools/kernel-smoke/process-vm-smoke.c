/* SPDX-License-Identifier: MIT */
/* actual ARM32 syscall and access-control tests. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/statfs.h>
#include <sys/sysinfo.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define PAGE 4096u
static unsigned checks, failures;
static void check(int pass, const char *name) {
    checks++;
    if (!pass) { failures++; printf("process-vm-smoke FAIL %s errno=%d\n", name, errno); }
}
static ssize_t pvm(int writing, pid_t pid, const struct iovec *l, unsigned long nl,
                   const struct iovec *r, unsigned long nr, unsigned long flags) {
    return syscall(writing ? 377L : 376L, (long)pid, l, nl, r, nr, flags);
}
static ssize_t one(int writing, pid_t pid, void *local, void *remote, size_t len) {
    struct iovec l={local,len},r={remote,len};
    return pvm(writing,pid,&l,1,&r,1,0);
}
static void *map(size_t length) {
    void *p=mmap(NULL,length,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(p==MAP_FAILED) { perror("process-vm mmap"); exit(2); }
    return p;
}
static int bytes_equal(const void *p, unsigned char value, size_t n) {
    const unsigned char *s=p;for(size_t i=0;i<n;i++)if(s[i]!=value)return 0;return 1;
}
static void expect_error(int writing,pid_t pid,struct iovec *l,unsigned long nl,
                         struct iovec *r,unsigned long nr,unsigned long flags,int wanted,const char *name) {
    errno=0;ssize_t rc=pvm(writing,pid,l,nl,r,nr,flags);
    check(rc==-1 && errno==wanted,name);
}
static void abi_tests(void) {
    unsigned char input[64],output[64];memset(input,0x5a,sizeof(input));
    struct iovec l={output,sizeof(output)},r={input,sizeof(input)},zero={(void *)-1,0};
    struct iovec badlen={input,0x80000000UL},local_zero={NULL,0};
    for(int w=0;w<2;w++) {
        memset(output,0x32,sizeof(output));
        check(one(w,getpid(),w?(void *)input:output,w?(void *)output:input,64)==64 &&
              !memcmp(input,output,64),"self-copy");
        expect_error(w,getpid(),&l,1,&r,1,1,EINVAL,"nonzero-flags");
        expect_error(w,getpid(),&l,1025,&r,1,0,EINVAL,"local-count-limit");
        expect_error(w,getpid(),&l,1,&r,1025,0,EINVAL,"remote-count-limit");
        expect_error(w,getpid(),&badlen,1,&r,1,0,EINVAL,"negative-local-length");
        expect_error(w,getpid(),&l,1,&badlen,1,0,EINVAL,"negative-remote-length");
        expect_error(w,getpid(),(void *)1,1,&r,1,0,EFAULT,"bad-local-array");
        expect_error(w,getpid(),&l,1,(void *)1,1,0,EFAULT,"bad-remote-array");
        expect_error(w,0x7fffffff,&l,1,&r,1,0,ESRCH,"missing-pid");
        check(pvm(w,0x7fffffff,NULL,0,(void *)1,1025,0)==0,"empty-local-short-circuit");
        check(pvm(w,0x7fffffff,&l,1,&zero,1,0)==0,"zero-remote-no-task");
        check(pvm(w,0x7fffffff,&local_zero,1,&r,1,0)==0,"zero-local-no-task");
        struct iovec wrap={(void *)0xfffffff0UL,128};
        expect_error(w,getpid(),&l,1,&wrap,1,0,EFAULT,"remote-address-overflow");
        struct iovec tail[2]={{input,7},{(void *)0xfffffff0UL,128}};
        check(pvm(w,getpid(),&l,1,tail,2,0)==7,"good-vector-before-overflow");
    }
}
static void vector_tests(void) {
    const size_t sizes[]={1,31,PAGE-1,PAGE,PAGE+1,16*PAGE,17*PAGE+7,2049*PAGE+19};
    for(unsigned n=0;n<sizeof(sizes)/sizeof(*sizes);n++) {
        size_t len=sizes[n];unsigned char *a=map(len+PAGE),*b=map(len+PAGE);
        for(size_t i=0;i<len+PAGE;i++)a[i]=(unsigned char)(i*137u+73u);
        for(int w=0;w<2;w++) {
            memset(b,0xd3,len+PAGE);
            check(one(w,getpid(),w?a+13:b+29,w?b+29:a+13,len)==(ssize_t)len &&
                  !memcmp(a+13,b+29,len) && b[28]==0xd3 && b[len+29]==0xd3,"unaligned-batch-copy");
        }
        munmap(a,len+PAGE);munmap(b,len+PAGE);
    }
    unsigned char a[512],b[512];for(unsigned i=0;i<sizeof(a);i++)a[i]=(unsigned char)i;
    for(int w=0;w<2;w++)for(int split=0;split<2;split++) {
        memset(b,0xd3,sizeof(b));
        struct iovec many_l[5]={{b,0},{b,3},{b+3,0},{b+3,133},{b+136,120}};
        struct iovec many_r[5]={{a,1},{a+1,0},{a+1,91},{a+92,164},{a+256,0}};
        struct iovec single_l={b,256},single_r={a,256};
        struct iovec *l=split?many_l:&single_l,*r=split?&single_r:many_r;
        unsigned long nl=split?5:1,nr=split?1:5;
        if(w) {struct iovec *v=l;l=r;r=v;unsigned long c=nl;nl=nr;nr=c;}
        check(pvm(w,getpid(),l,nl,r,nr,0)==256 && !memcmp(a,b,256) && b[256]==0xd3,"vector-segmentation");
    }
    for(int w=0;w<2;w++)for(int local_short=0;local_short<2;local_short++) {
        memset(b,0xd3,sizeof(b));
        struct iovec l={w?a:b,local_short?37:400},r={w?b:a,local_short?400:37};
        check(pvm(w,getpid(),&l,1,&r,1,0)==37 && !memcmp(a,b,37) && b[37]==0xd3,"shorter-side-stops-copy");
    }
    struct iovec many[1024];for(unsigned i=0;i<1024;i++)many[i]=(struct iovec){a,i==1023?64:0};
    struct iovec dst={b,64};memset(b,0,64);
    check(pvm(0,getpid(),&dst,1,many,1024,0)==64&&!memcmp(a,b,64),"maximum-vector-count");
}
static void fault_tests(void) {
    unsigned char *remote=map(3*PAGE),*local=map(3*PAGE);
    for(int w=0;w<2;w++) {
        memset(remote,0x6c,3*PAGE);memset(local,0x27,3*PAGE);
        check(mprotect(remote+PAGE,PAGE,PROT_NONE)==0,"protect-middle-remote");
        check(one(w,getpid(),local,remote,3*PAGE)==PAGE,"partial-remote-pin-prefix");
        check(bytes_equal(w?remote:local,w?0x27:0x6c,PAGE),"remote-prefix-data");
        check(bytes_equal(remote+2*PAGE,0x6c,PAGE)&&bytes_equal(local+2*PAGE,0x27,PAGE),"remote-later-page-untouched");
        struct iovec l={local,3*PAGE},r[3]={{remote,PAGE},{remote+PAGE,PAGE},{remote+2*PAGE,PAGE}};
        check(pvm(w,getpid(),&l,1,r,3,0)==PAGE,"partial-remote-vector-prefix");
        errno=0;check(one(w,getpid(),local,remote+PAGE,32)==-1&&errno==EFAULT,"remote-first-byte-fault");
        check(mprotect(remote+PAGE,PAGE,PROT_READ|PROT_WRITE)==0,"unprotect-remote");
    }
    memset(remote,0x75,3*PAGE);memset(local,0x31,3*PAGE);
    check(munmap(local+PAGE,PAGE)==0,"unmap-local-source");
    /* Copy 64 good bytes then fault inside ONE remote destination page. */
    ssize_t rc=one(1,getpid(),local+PAGE-64,remote+17,128);
    check(rc==64,"local-write-short-copy");
    check(bytes_equal(remote+17,0x31,64)&&bytes_equal(remote+81,0x75,47),"write-fault-tail-not-zeroed");
    errno=0;check(one(1,getpid(),local+PAGE,remote+200,128)==-1&&errno==EFAULT,"local-first-byte-write-fault");
    check(bytes_equal(remote+200,0x75,128),"first-fault-remote-unchanged");
    rc=one(0,getpid(),local+PAGE-64,remote,128);
    check(rc==64 && !memcmp(local+PAGE-64,remote,64),"local-read-short-copy");
    check(mmap(local+PAGE,PAGE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==local+PAGE,"restore-local-page");
    check(mprotect(remote,PAGE,PROT_READ)==0,"remote-readonly");
    check(one(0,getpid(),local,remote,64)==64,"read-readonly-remote");
    errno=0;check(one(1,getpid(),local,remote,64)==-1&&errno==EFAULT,"write-readonly-remote-denied");
    munmap(remote,3*PAGE);munmap(local,3*PAGE);
}
static volatile sig_atomic_t signal_ok;
static void signal_handler(int sig) {
    (void)sig;int input=0x178921,output=0;
    ssize_t rc=one(0,getpid(),&output,&input,sizeof(input));
    signal_ok=(rc==(ssize_t)sizeof(input)&&output==input);
}
static void *thread_read(void *unused) {
    (void)unused;int src=0x4567,dst=0;
    return (void *)(uintptr_t)(one(0,getpid(),&dst,&src,sizeof(src))==(ssize_t)sizeof(src)&&dst==src);
}
static void context_tests(void) {
    pthread_t t;void *answer=NULL;
    int rc=pthread_create(&t,NULL,thread_read,NULL);check(rc==0,"create-thread");
    if(!rc)check(pthread_join(t,&answer)==0&&answer==(void *)1,"nonleader-self-read");
    struct sigaction sa;memset(&sa,0,sizeof(sa));sa.sa_handler=signal_handler;
    sigemptyset(&sa.sa_mask);check(sigaction(SIGUSR1,&sa,NULL)==0,"signal-handler-install");
    check(raise(SIGUSR1)==0&&signal_ok,"signal-handler-self-read");
}
static void credentials_tests(void) {
    /* Each target and caller live only in this RAM-root test VM. */
    unsigned char *remote=map(PAGE);memset(remote,0x6b,PAGE);
    for(int mode=0;mode<3;mode++) {
        int ready[2],done[2];check(pipe(ready)==0&&pipe(done)==0,"credential-pipes");
        pid_t child=fork();check(child>=0,"credential-target-fork");if(child<0)exit(2);
        if(!child) {
            close(ready[0]);close(done[1]);
            if(setgroups(0,NULL)||setgid(mode==2?10001:10000)||setuid(mode==2?10001:10000)||
               prctl(PR_SET_DUMPABLE,mode==1?0:1))_exit(2);
            if(write(ready[1],"R",1)!=1)_exit(3);
            char c;if(read(done[0],&c,1)!=1)_exit(4);
            _exit(0);
        }
        close(ready[1]);close(done[0]);char c;
        check(read(ready[0],&c,1)==1&&c=='R',"target-credential-ready");
        /* Root CAP_SYS_PTRACE permits DAC checks; no SELinux policy here. */
        unsigned char local[16];memset(local,0x6b,sizeof(local));
        for(int w=0;w<2;w++)check(one(w,child,local,remote,16)==16,"root-ptrace-capability");
        pid_t reader=fork();check(reader>=0,"credential-reader-fork");if(reader<0)exit(2);
        if(!reader) {
            if(setgroups(0,NULL)||setgid(10000)||setuid(10000))_exit(2);
            unsigned char data[16];memset(data,0x6b,sizeof(data));
            for(int w=0;w<2;w++) {
                errno=0;ssize_t rc=one(w,child,data,remote,16);
                if(mode==0) {if(rc!=16||!bytes_equal(data,0x6b,16))_exit(3);}
                else if(rc!=-1||errno!=EPERM)_exit(4);
            }
            _exit(0);
        }
        int status;check(waitpid(reader,&status,0)==reader&&WIFEXITED(status)&&WEXITSTATUS(status)==0,
                         mode==0?"same-uid-allowed":mode==1?"nondumpable-denied-both":"different-uid-denied-both");
        check(write(done[1],"D",1)==1,"release-target");
        check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"credential-target-exit");
        close(ready[0]);close(done[1]);
        struct iovec l={local,16},r={remote,16};expect_error(0,child,&l,1,&r,1,0,ESRCH,"exited-target");
    }
    munmap(remote,PAGE);
}
static void cow_tests(void) {
    const char *name="/tmp/process-vm-cow";int fd=open(name,O_RDWR|O_CREAT|O_EXCL,0600);
    check(fd>=0,"cow-file-open");if(fd<0)return;
    unsigned char page[PAGE];memset(page,0x17,PAGE);
    check(write(fd,page,PAGE)==PAGE,"cow-file-fill");
    unsigned char *private=mmap(NULL,PAGE,PROT_READ|PROT_WRITE,MAP_PRIVATE,fd,0);
    unsigned char *shared=mmap(NULL,PAGE,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    check(private!=MAP_FAILED&&shared!=MAP_FAILED,"cow-mappings");
    if(private!=MAP_FAILED&&shared!=MAP_FAILED) {
        unsigned char data[19];memset(data,0x92,sizeof(data));
        check(one(1,getpid(),data,private+23,sizeof(data))==(ssize_t)sizeof(data),"private-file-write");
        check(bytes_equal(private+23,0x92,sizeof(data))&&bytes_equal(shared,0x17,PAGE),"cow-original-unchanged");
        check(pread(fd,page,PAGE,0)==PAGE&&bytes_equal(page,0x17,PAGE),"cow-file-unchanged");
        check(one(1,getpid(),data,shared+43,sizeof(data))==(ssize_t)sizeof(data),"shared-short-write");
        check(msync(shared,PAGE,MS_SYNC)==0,"shared-msync");
        check(pread(fd,page,PAGE,0)==PAGE&&bytes_equal(page+43,0x92,sizeof(data))&&page[42]==0x17&&page[62]==0x17,"shared-file-data");
        munmap(private,PAGE);munmap(shared,PAGE);
    }
    close(fd);unlink(name);
}
static void racing_tests(void) {
    unsigned char *r=map(2*PAGE),*l=map(2*PAGE);memset(r,0x4a,2*PAGE);
    int ready[2];if(pipe(ready))exit(2);
    pid_t child=fork();if(child<0)exit(2);
    if(!child) {
        close(ready[0]);if(write(ready[1],"R",1)!=1)_exit(2);
        for(int i=0;i<400;i++) {
            if(mprotect(r,2*PAGE,PROT_NONE)||mprotect(r,2*PAGE,PROT_READ|PROT_WRITE))_exit(3);
            if(munmap(r+PAGE,PAGE))_exit(4);
            if(mmap(r+PAGE,PAGE,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)!=r+PAGE)_exit(5);
        }
        _exit(0);
    }
    close(ready[1]);char c;check(read(ready[0],&c,1)==1,"race-ready");close(ready[0]);
    for(int i=0;i<800;i++)for(int w=0;w<2;w++) {
        errno=0;ssize_t rc=one(w,child,l,r,2*PAGE);
        check((rc>=0&&rc<=(ssize_t)(2*PAGE))||(rc==-1&&(errno==EFAULT||errno==ESRCH)),"mprotect-unmap-exit-race-bounded");
    }
    int status;check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0,"race-target-clean-exit");
    munmap(r,2*PAGE);munmap(l,2*PAGE);
}
static void release_tests(void) {
    struct sysinfo before,after;check(sysinfo(&before)==0,"free-memory-before");
    const size_t len=2*1024*1024;
    for(int n=0;n<24;n++) {
        unsigned char *a=map(len),*b=map(len);memset(a,0x19,len);memset(b,0x46,len);
        check(one(0,getpid(),b,a,len)==(ssize_t)len,"release-read-batch");
        check(one(1,getpid(),b,a,len)==(ssize_t)len,"release-write-batch");
        check(munmap(b,len)==0,"release-fault-unmap");
        errno=0;check(one(1,getpid(),b,a,len)==-1&&errno==EFAULT,"release-fault-pins");
        check(munmap(a,len)==0,"release-buffers");
    }
    check(sysinfo(&after)==0,"free-memory-after");
    unsigned long long first=(unsigned long long)before.freeram*before.mem_unit;
    unsigned long long last=(unsigned long long)after.freeram*after.mem_unit;
    printf("process-vm-smoke free_before=%llu free_after=%llu tolerance=4194304\n",first,last);
    check(last+4*1024*1024>=first,"bounded-pin-release-memory");
}
int main(int argc,char **argv) {
    struct utsname uts;struct statfs fs;
    if(argc!=2||strcmp(argv[1],"--run-isolated")||getuid()!=0||getppid()!=1||
       uname(&uts)||strcmp(uts.machine,"armv7l")||!strstr(uts.release,"-pw3-core-smoke")||statfs("/",&fs)||
       ((unsigned long)fs.f_type!=0x858458f6UL&&(unsigned long)fs.f_type!=0x01021994UL)) {
        fputs("process-vm-smoke: isolated ARM RAM-root guard refused\n",stderr);return 77;
    }
    setvbuf(stdout,NULL,_IONBF,0);alarm(30);
    abi_tests();vector_tests();fault_tests();context_tests();credentials_tests();cow_tests();racing_tests();release_tests();
    printf("process-vm-smoke SUMMARY: checks=%u failed=%u\n",checks,failures);
    puts(failures?"PW3_PROCESS_VM_FAIL":"PW3_PROCESS_VM_PASS");
    return failures?1:0;
}
