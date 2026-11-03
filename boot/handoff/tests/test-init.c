/* SPDX-License-Identifier: GPL-2.0-only */
/* Diskless VExpress PID1 fixture, never a Kindle launcher. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>
#include "stage_abi.h"
#include "native-abi.h"
#include "sha256.h"
static unsigned checks;
#ifndef SECONDARY
extern int pws_engine_guard_test(void);
extern int pws_identity_attrs_test(void);
extern int pws_allocation_ranges_test(void);
#endif
static void end(int ok)
{
#ifdef SECONDARY
    printf("PW3_SECOND_KERNEL_%s checks=%u\n",ok?"PASS":"FAIL",checks);
#else
    printf("PW3_FIRST_KERNEL_FAIL checks=%u unexpected_return=%d\n",checks,ok);
#endif
    fflush(stdout);sync();reboot(RB_POWER_OFF);for(;;)pause();
}
#define REQUIRE(x) do{++checks;if(!(x)){printf("FAIL line=%u errno=%d\n",__LINE__,errno);end(0);}}while(0)
static void read_text(const char *path,char *out,size_t limit)
{
    int fd=open(path,O_RDONLY);REQUIRE(fd>=0);
    ssize_t n=read(fd,out,limit-1);REQUIRE(n>=0&&(size_t)n<limit-1);
    out[n]=0;REQUIRE(close(fd)==0);
}
#ifndef SECONDARY
static void *read_blob(const char *path,unsigned *bytes)
{
    int fd=open(path,O_RDONLY);struct stat st;REQUIRE(fd>=0&&fstat(fd,&st)==0);
    REQUIRE(S_ISREG(st.st_mode)&&st.st_size>0&&st.st_size<=40*1024*1024);
    unsigned n=(unsigned)st.st_size;unsigned char *p=malloc(n);REQUIRE(p);
    for(unsigned off=0;off<n;) {ssize_t r=read(fd,p+off,n-off);REQUIRE(r>0);off+=r;}
    REQUIRE(close(fd)==0);*bytes=n;return p;
}
/* Declare, copy and seal one set of slots; 0, or the errno SEAL failed with
 * (the module has then discarded everything). Prepares after a good seal. */
static int upload(int fd,void *const *bytes,const unsigned *sizes)
{
    struct pws_begin b={.version=1};
    for(unsigned i=0;i<3;++i) {
        SHA256_CTX h;sha256_init(&h);sha256_update(&h,bytes[i],sizes[i]);
        sha256_final(&h,b.images[i].sha256);b.images[i].bytes=sizes[i];
    }
    REQUIRE(ioctl(fd,PWS_BEGIN,&b)==0);
    for(unsigned i=0;i<3;++i)for(unsigned off=0;off<sizes[i];) {
        unsigned n=sizes[i]-off;if(n>65536)n=65536;
        struct pws_put p={.slot=i,.offset=off,.bytes=n,
            .user_buffer=(uintptr_t)((char *)bytes[i]+off)};
        REQUIRE(ioctl(fd,PWS_PUT,&p)==0);off+=n;
    }
    if(ioctl(fd,PWS_SEAL,0))return errno;
    REQUIRE(ioctl(fd,PWS_PREPARE,0)==0);
    return 0;
}
/* The initramfs slot as the launcher makes it: the public archive, zero
 * padding to a multiple of four, then the device archive (`gap` more zero
 * bytes, or one junk byte with gap<0, to test the module's rules). */
static void *initramfs_slot(const void *pub,unsigned pub_bytes,const void *dev,unsigned dev_bytes,int gap,unsigned *bytes)
{
    unsigned start=(pub_bytes+3u)&~3u,extra=gap>0?(unsigned)gap:0;
    unsigned char *p=calloc(1,start+extra+dev_bytes);REQUIRE(p);
    memcpy(p,pub,pub_bytes);memcpy(p+start+extra,dev,dev_bytes);
    if(gap<0)p[pub_bytes]=1;
    *bytes=start+extra+dev_bytes;return p;
}
#endif
int main(void)
{
    struct utsname u;char text[8192];
    if(getpid()!=1||getuid()!=0||uname(&u)||!strstr(u.release,"-pw3-core-smoke"))return 77;
    REQUIRE(mount("proc","/proc","proc",0,NULL)==0);
    REQUIRE(mount("sysfs","/sys","sysfs",0,NULL)==0);
    read_text("/proc/cpuinfo",text,sizeof(text));REQUIRE(strstr(text,"ARM-Versatile Express"));
    read_text("/proc/cmdline",text,sizeof(text));
#ifdef SECONDARY
    REQUIRE(strstr(text,"pw3_virtual_stage=second"));
    read_text("/proc/modules",text,sizeof(text));REQUIRE(!strstr(text,"pw3_first_kernel"));
    struct stat st;REQUIRE(stat("/target-initramfs-marker",&st)==0&&S_ISREG(st.st_mode));
    REQUIRE(stat("/pw3_first_kernel.ko",&st)<0&&errno==ENOENT);
    read_text("/proc/mounts",text,sizeof(text));
    REQUIRE(!strstr(text,"/dev/mmc")&&!strstr(text,"/dev/sd"));
    end(1);
#else
    REQUIRE(strstr(text,"pw3_virtual_stage=first"));
    REQUIRE(pws_engine_guard_test()==0);
    REQUIRE(pws_identity_attrs_test()==0);
    REQUIRE(pws_allocation_ranges_test()==0);
    unsigned n;void *provider=read_blob("/pw3_qemu_pm.ko",&n);
    REQUIRE(syscall(SYS_init_module,provider,(size_t)n,"")==0);free(provider);
    void *module=read_blob("/pw3_first_kernel.ko",&n);
    REQUIRE(syscall(SYS_init_module,module,(size_t)n,"")==0);free(module);
    read_text("/sys/class/misc/pw3-first-kernel/dev",text,sizeof(text));
    unsigned major_num,minor_num;REQUIRE(sscanf(text,"%u:%u",&major_num,&minor_num)==2);
    REQUIRE(mknod("/dev/pw3-first-kernel",S_IFCHR|0600,makedev(major_num,minor_num))==0);
    int fd=open("/dev/pw3-first-kernel",O_RDWR);REQUIRE(fd>=0);
    errno=0;REQUIRE(ioctl(fd,PWS_NATIVE_COMMIT,0)==-1&&errno==EPERM);
    /* The build's kernel and public initramfs, the device's archive (here the
     * observer's RAM FAT image, which the second Linux needs: it starts only
     * if the appended archive was unpacked) and ATAGs. */
    unsigned kb,ab,pb,db,bad_tag_b,bad_cmd_b,pub_only_b,ib;
    void *kernel=read_blob("/target-zImage",&kb),*atags=read_blob("/target-atags",&ab);
    void *pub=read_blob("/target-public-initramfs",&pb),*dev=read_blob("/target-device-archive",&db);
    void *bad_tag=read_blob("/target-atags-extra-tag",&bad_tag_b),*bad_cmd=read_blob("/target-atags-other-cmdline",&bad_cmd_b);
    void *pub_only=read_blob("/target-atags-public-only",&pub_only_b);
    void *initramfs=initramfs_slot(pub,pb,dev,db,0,&ib);
    void *good[3]={kernel,atags,initramfs};unsigned good_sizes[3]={kb,ab,ib};
    /* Allocation failure remains terminal and fully releases every owned
     * page; its reason survives cleanup and the next begin clears it. */
    {int f=open("/sys/module/pw3_first_kernel/parameters/fail_alloc_after",O_WRONLY);REQUIRE(f>=0 && write(f,"2",1)==1 && !close(f));}
    {struct pws_begin b={.version=1};for(unsigned i=0;i<3;++i)b.images[i].bytes=good_sizes[i];
     errno=0;REQUIRE(ioctl(fd,PWS_BEGIN,&b)==-1 && errno==ENOMEM);}
    struct pws_status failed={0};REQUIRE(ioctl(fd,PWS_STATUS,&failed)==0 && failed.state==PWS_EMPTY && !failed.payload_pages && !failed.control_pages && !failed.collision_pages);
    read_text("/sys/module/pw3_first_kernel/parameters/live_pages",text,sizeof(text));REQUIRE(!strcmp(text,"0\n"));
    read_text("/sys/module/pw3_first_kernel/parameters/allocation_failure_reason",text,sizeof(text));REQUIRE(!strcmp(text,"4\n"));
    {int f=open("/sys/module/pw3_first_kernel/parameters/fail_alloc_after",O_WRONLY);REQUIRE(f>=0 && write(f,"-1",2)==2 && !close(f));}
    /* What the build pins: a kernel or public initramfs other than the build's
     * is refused even when the caller declares its hash. */
    ((unsigned char *)kernel)[0x100]^=1;REQUIRE(upload(fd,good,good_sizes)==EBADMSG);((unsigned char *)kernel)[0x100]^=1;
    read_text("/sys/module/pw3_first_kernel/parameters/allocation_failure_reason",text,sizeof(text));REQUIRE(!strcmp(text,"0\n"));
    ((unsigned char *)initramfs)[pb/2]^=1;REQUIRE(upload(fd,good,good_sizes)==EBADMSG);((unsigned char *)initramfs)[pb/2]^=1;
    /* What the device adds is held to the build's rules. */
    {void *v[3]={kernel,bad_tag,initramfs};unsigned n[3]={kb,bad_tag_b,ib};REQUIRE(upload(fd,v,n)==ENOEXEC);}
    {void *v[3]={kernel,bad_cmd,initramfs};unsigned n[3]={kb,bad_cmd_b,ib};REQUIRE(upload(fd,v,n)==ENOEXEC);}
    {void *v[3]={kernel,pub_only,pub};unsigned n[3]={kb,pub_only_b,pb};REQUIRE(upload(fd,v,n)==ENOEXEC);}
    {unsigned jb;void *junk=initramfs_slot(pub,pb,dev,db,pb%4?-1:4,&jb);
     /* The ATAGs must name the slot's actual size: with the same ATAGs a
      * longer slot fails the plan, a junk byte the archive rule. */
     void *v[3]={kernel,atags,junk};unsigned n[3]={kb,ab,jb};REQUIRE(upload(fd,v,n)==ENOEXEC);free(junk);}
    /* The real thing. */
    REQUIRE(upload(fd,good,good_sizes)==0);
    errno=0;REQUIRE(ioctl(fd,PWS_NATIVE_COMMIT,1)==-1&&errno==EINVAL);
    errno=0;REQUIRE(ioctl(fd,PWS_NATIVE_COMMIT,0)==-1&&errno==EPERM);
    struct pws_native_report r={0};
    /* Regression: a private page is not a valid Linux task stack. The same
     * backend validator must reject it before any cache callback. */
    {int f=open("/sys/module/pw3_first_kernel/parameters/native_test_stack",O_WRONLY);REQUIRE(f>=0 && write(f,"1",1)==1 && !close(f));}
    REQUIRE(ioctl(fd,PWS_NATIVE_PROBE,0)==0);
    REQUIRE(ioctl(fd,PWS_NATIVE_REPORT,&r)==0 && r.version==4 && r.backend_check_performed==1 && r.backend_result==-6 && r.callback_rejected && r.resume_samples==1 && !r.pm_result);
    errno=0;REQUIRE(ioctl(fd,PWS_NATIVE_COMMIT,0)==-1 && errno==EPERM);
    {int f=open("/sys/module/pw3_first_kernel/parameters/native_test_stack",O_WRONLY);REQUIRE(f>=0 && write(f,"0",1)==1 && !close(f));}
    for(unsigned round=0;round<3;round++){
        REQUIRE(ioctl(fd,PWS_NATIVE_PROBE,0)==0);
        REQUIRE(ioctl(fd,PWS_NATIVE_REPORT,&r)==0);
        REQUIRE(r.version==4 && r.profile==2 && r.pm_called==1 && !r.commit_requested && r.suspend_samples==1 && r.resume_samples==1 && r.irq_fiq_masked && r.cpu_profile_valid && r.registers_valid && r.gates_clear && !r.hardware_sampled && r.returned_to_caller && !r.pm_result && r.backend_check_performed && !r.backend_result && !r.callback_rejected);
        if(round==0){
            REQUIRE(r.dcp_stopped);
            int f=open("/sys/module/pw3_first_kernel/parameters/native_test_reject",O_WRONLY);REQUIRE(f>=0 && write(f,"1",1)==1 && !close(f));
            errno=0;REQUIRE(ioctl(fd,PWS_NATIVE_COMMIT,0)==-1 && errno==EBUSY);
            REQUIRE(ioctl(fd,PWS_NATIVE_REPORT,&r)==0 && r.commit_requested && r.callback_rejected && !r.dcp_stopped && r.suspend_samples==1 && !r.resume_samples && r.returned_to_caller && r.pm_result==-EBUSY);
            errno=0;REQUIRE(ioctl(fd,PWS_NATIVE_COMMIT,0)==-1 && errno==EPERM);
        }else if(round==1){
            REQUIRE(!r.dcp_stopped);errno=0;REQUIRE(ioctl(fd,PWS_NATIVE_COMMIT,0)==-1 && errno==EPERM);
            int f=open("/sys/module/pw3_first_kernel/parameters/native_test_reject",O_WRONLY);REQUIRE(f>=0 && write(f,"0",1)==1 && !close(f));
        }else REQUIRE(r.dcp_stopped);
    }
    printf("PW3_FIRST_KERNEL_READY checks=%u native_core_roundtrips=4 rejection_checks=8\n",checks);fflush(stdout);
    int rc=ioctl(fd,PWS_NATIVE_COMMIT,0);
    printf("PW3_HANDOFF_RETURN rc=%d errno=%d\n",rc,errno);end(0);
#endif
    return 1;
}
