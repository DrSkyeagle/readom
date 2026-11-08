#include <sys/vfs.h>
#include "pm-logic.h"
#define SYSFS_MAGIC 0x62656572
static char log_before[131073],log_after[131073];
static int read_pm(const char *path,char *text,size_t size) {
    int fd=open(path,O_RDONLY|O_NOFOLLOW|O_CLOEXEC);struct statfs fs;
    if(fd<0)return 0;
    if(fstatfs(fd,&fs) || fs.f_type!=SYSFS_MAGIC){close(fd);return 0;}
    ssize_t n=read(fd,text,size-1);int saved=errno;close(fd);errno=saved;
    if(n<=0 || (size_t)n>=size-1)return 0;
    text[n]=0;return 1;
}
static int write_pm(const char *path,const char *value) {
    int fd=open(path,O_WRONLY|O_NOFOLLOW|O_CLOEXEC);struct statfs fs;
    if(fd<0)return 0;
    if(fstatfs(fd,&fs) || fs.f_type!=SYSFS_MAGIC){close(fd);return 0;}
    size_t bytes=strlen(value);ssize_t n=write(fd,value,bytes);int saved=errno;
    int closed=close(fd);errno=saved;return n==(ssize_t)bytes && !closed;
}
static int restore_pm_test(void) {
    char mode[256];
    if(!write_pm("/sys/power/pm_test","none\n") || !read_pm("/sys/power/pm_test",mode,sizeof(mode)) || !selected_level(mode,"none"))return 0;
    pm_test_changed=0;return 1;
}
static void preflight_pm(void) {
    char text[256];
    require(read_pm("/sys/power/pm_test",text,sizeof(text)),"pm_test_sysfs_available");
    require(selected_level(text,"none") && token_present(text,"core"),"native_pm_modes_and_original_none");
    require(read_pm("/sys/power/state",text,sizeof(text)) && token_present(text,"mem"),"mem_suspend_supported");
    int fd=open("/sys/module/g_file_storage/parameters/file",O_RDONLY|O_NOFOLLOW|O_CLOEXEC);
    if(fd>=0){ssize_t n=read(fd,text,sizeof(text)-1);close(fd);require(n>=0 && (size_t)n<sizeof(text)-1,"usb_backing_read");text[n]=0;for(ssize_t i=0;i<n;i++)require(text[i]=='\n'||text[i]==' '||text[i]==',',"no_exported_usb_backing_store");}
    else require(errno==ENOENT,"usb_backing_absent_or_empty");
}
static void pm_test_level(const char *level) {
    char text[256];
    require(!pm_test_changed && read_pm("/sys/power/pm_test",text,sizeof(text)) && selected_level(text,"none"),"pm_mode_still_none");
    pm_test_changed=1;
    require(write_pm("/sys/power/pm_test",level),"select_native_pm_test");
    require(read_pm("/sys/power/pm_test",text,sizeof(text)) && selected_level(text,level),"native_pm_mode_readback");
}
static void read_kernel_log(char *buffer,size_t capacity) {
    int n=klogctl(3,buffer,(int)capacity-1);require(n>128 && n<(int)capacity,"kernel_log_read");buffer[n]=0;
}
static long long elapsed_ms(const struct timespec *begin,const struct timespec *end) {
    return ((long long)end->tv_sec-begin->tv_sec)*1000+(end->tv_nsec-begin->tv_nsec)/1000000;
}
static void run_native_cycle(int commit) {
    const char *phase=commit?"commit":"core";char filename[80],record[4096],mode[256];struct timespec begin,end;
    struct pws_native_report r={0};
    pm_test_level("core");
    snprintf(filename,sizeof(filename),"%s-attempt.txt",phase);
    write_result(filename,commit?"phase=commit\nnew_kernel_success_unproven=1\n":"phase=core-roundtrip\nnew_kernel_attempted=0\n");
    read_kernel_log(log_before,sizeof(log_before));snprintf(filename,sizeof(filename),"%s-before-kernel.txt",phase);write_kernel_log(filename,log_before);
    require(fflush(stdout)==0 && fsync(STDOUT_FILENO)==0,"sync_launcher_log_before_pm");sync();
    require(read_pm("/sys/power/pm_test",mode,sizeof(mode)) && selected_level(mode,"core"),"core_immediately_before_cycle");
    require(clock_gettime(CLOCK_MONOTONIC,&begin)==0,"clock_before_native_cycle");
    /* The last look at the cancel flag before the point of no return. Everything
     * between the caller's checkpoint and here -- two files written, the kernel
     * log read, an fsync and a global sync -- takes long enough on this storage
     * for a stop request or the deadline to arrive inside it, and the commit
     * would have gone ahead with one waiting. */
    if(commit) require(!interrupted,"interrupted_or_deadline");
    /* And then the deadline is taken off the table rather than left to chance.
     * Inside the commit the kernel syncs filesystems, freezes processes and
     * suspends devices before the jump, and a SIGALRM landing in there would
     * set the cancel flag with nobody left to read it -- this program is not
     * running to check it, and the module does not. The start would go ahead
     * anyway, which is the right answer (what remains returns in seconds or
     * not at all, and tearing down a prepared handover costs more than
     * waiting), so it is the deliberate answer here instead of the one that
     * depends on when the signal arrives. Armed again after it returns: a
     * commit that comes back still has files to write. */
    if(commit) alarm(0);
    errno=0;int rc=ioctl(control,commit?PWS_NATIVE_COMMIT:PWS_NATIVE_PROBE,0),saved=errno;
    if(commit) alarm(90);
    int clock_ok=clock_gettime(CLOCK_MONOTONIC,&end)==0;
    int restored=restore_pm_test();
    int report_ok=ioctl(control,PWS_NATIVE_REPORT,&r)==0;
    read_kernel_log(log_after,sizeof(log_after));snprintf(filename,sizeof(filename),"%s-after-kernel.txt",phase);write_kernel_log(filename,log_after);
    long long ms=clock_ok?elapsed_ms(&begin,&end):-1;
    int n=snprintf(record,sizeof(record),"schema=pw3-native-first-kernel-result-v3\nphase=%s\ncycle_return=%d\ncycle_errno=%d\npm_test_restored=%d\nelapsed_ms=%lld\nclock_excludes_suspend=1\nreport_read=%d\nversion=%u\nprofile=%u\ncommit_requested=%u\npm_called=%u\nsuspend_samples=%u\nresume_samples=%u\nirq_fiq_masked=%u\ncpu_profile_valid=%u\nregisters_valid=%u\nhardware_sampled=%u\ngates_clear=%u\ndcp_stopped=%u\ndcp_reset=%u\ndcp_clock_gated=%u\ncallback_rejected=%u\nreturned_to_caller=%u\npm_result=%d\nbackend_result=%d\n",phase,rc,rc?saved:0,restored,ms,report_ok,r.version,r.profile,r.commit_requested,r.pm_called,r.suspend_samples,r.resume_samples,r.irq_fiq_masked,r.cpu_profile_valid,r.registers_valid,r.hardware_sampled,r.gates_clear,r.dcp_stopped,r.dcp_reset,r.dcp_clock_gated,r.callback_rejected,r.returned_to_caller,r.pm_result,r.backend_result);
    require(n>0 && n<(int)sizeof(record),"report_capacity");
    for(unsigned i=0;i<PWS_GATE_COUNT;i++){int k=snprintf(record+n,sizeof(record)-(size_t)n,"gate_%u=%u\n",i,r.gate[i]);require(k>0 && k<(int)(sizeof(record)-(size_t)n),"gate_report_capacity");n+=k;}
    {
        int k=snprintf(record+n,sizeof(record)-(size_t)n,"diagnostic_only=%u\ndcp_ctrl=%u\ndcp_stat_activity=%u\ndcp_channel_enable=%u\ndcp_csc_enable=%u\nusb_sampled=%u\n",r.diagnostic_only,r.dcp_ctrl,r.dcp_stat_activity,r.dcp_channel_enable,r.dcp_csc_enable,r.usb_sampled);require(k>0 && k<(int)(sizeof(record)-(size_t)n),"extra_report_capacity");n+=k;
        for(unsigned i=0;i<4;i++){k=snprintf(record+n,sizeof(record)-(size_t)n,"dcp_sema_%u=%u\n",i,r.dcp_sema[i]);require(k>0 && k<(int)(sizeof(record)-(size_t)n),"sema_report_capacity");n+=k;}
        for(unsigned i=0;i<2;i++){
            k=snprintf(record+n,sizeof(record)-(size_t)n,"usb_%u_id=%u\nusb_%u_cap=%u\nusb_%u_dciversion=%u\nusb_%u_dccparams=%u\nusb_%u_command=%u\nusb_%u_status=%u\nusb_%u_mode=%u\nusb_%u_prime=%u\nusb_%u_flush=%u\nusb_%u_endpoints=%u\n",i,r.usb[i].id,i,r.usb[i].cap,i,r.usb[i].dciversion,i,r.usb[i].dccparams,i,r.usb[i].command,i,r.usb[i].status,i,r.usb[i].mode,i,r.usb[i].prime,i,r.usb[i].flush,i,r.usb[i].endpoints);require(k>0 && k<(int)(sizeof(record)-(size_t)n),"usb_report_capacity");n+=k;
        }
    }
    {int k=snprintf(record+n,sizeof(record)-(size_t)n,"usb_idle_observed=%u\ndcp_idle_observed=%u\nengine_guard_passed=%d\nbackend_check_performed=%u\n",r.usb_idle_observed,r.dcp_idle_observed,pws_engine_fields_ok(&r),r.backend_check_performed);require(k>0 && k<(int)(sizeof(record)-(size_t)n),"guard_report_capacity");n+=k;}
    snprintf(filename,sizeof(filename),"%s-return.txt",phase);write_result(filename,record);
    require(restored,"restore_pm_none");require(!commit,"commit_returned_without_new_kernel");require(rc==0 && report_ok,"native_core_cycle_return");
    require(clock_ok && ms>=0,"native_core_clock_read"); /* Monotonic excludes time spent in syscore suspend. */
    const char *added=new_log_suffix(log_before,log_after);
#if defined(PW3_EMULATOR)
    /* The emulator build's probe is synthesised in the module
     * (native-handoff.h native_probe_synthetic): the machine is not suspended,
     * so the kernel log carries no "suspend debug: Waiting for 5 seconds." line
     * and no registers were sampled. What still has to be true is that the call
     * returned a report of the shape the helper reads, with no error in it and
     * nothing reported from a machine that was never asked. This whole branch
     * exists only in test entries; a release build never defines PW3_EMULATOR,
     * and build_launcher.py refuses a module built one way against a helper
     * built the other. */
    require(!added || !pm_log_has_error(added),"emulator_core_no_reported_error");
    require(r.version==4 && r.profile==CHECK_PROFILE && !r.commit_requested && r.pm_called==1 &&
        r.suspend_samples==1 && r.resume_samples==1 && r.returned_to_caller &&
        !r.callback_rejected && !r.pm_result && !r.backend_result && r.backend_check_performed==1 &&
        pws_engine_fields_ok(&r),"emulator_core_probe_report");
#else
    require(added && strstr(added,"suspend debug: Waiting for 5 seconds.") && !pm_log_has_error(added),"native_core_checkpoint_and_no_reported_error");
    require(r.version==4 && r.profile==CHECK_PROFILE && !r.commit_requested && r.pm_called==1 && r.suspend_samples==1 && r.resume_samples==1 && r.irq_fiq_masked && r.cpu_profile_valid && r.registers_valid && r.hardware_sampled==1 && !r.callback_rejected && r.returned_to_caller && !r.pm_result && !r.backend_result && r.backend_check_performed==1 && r.diagnostic_only==0 && pws_engine_fields_ok(&r),"hardware_core_preflight_pass");
#endif

}
