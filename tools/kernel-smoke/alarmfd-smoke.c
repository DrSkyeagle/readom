/* Run only on the actual kernel under test; this does not enter suspend.
 * Linux target: cc -O2 -Wall -Wextra -o alarmfd-smoke alarmfd-smoke.c
 * Alarm clocks require CAP_WAKE_ALARM. Test execution is not part of generation.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/prctl.h>
#include <sys/timerfd.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#ifndef CLOCK_BOOTTIME
#define CLOCK_BOOTTIME 7
#endif
#ifndef CLOCK_REALTIME_ALARM
#define CLOCK_REALTIME_ALARM 8
#endif
#ifndef CLOCK_BOOTTIME_ALARM
#define CLOCK_BOOTTIME_ALARM 9
#endif
#ifndef TFD_TIMER_CANCEL_ON_SET
#define TFD_TIMER_CANCEL_ON_SET (1 << 1)
#endif
#ifndef EPOLLWAKEUP
#define EPOLLWAKEUP (1 << 29)
#endif

static int exercise(clockid_t clockid, const char *name)
{
    int failed=1, fd=-1, ep=-1;
    uint64_t expirations=0;
    struct itimerspec spec={0};
    struct epoll_event event={0};
    fd=timerfd_create(clockid,TFD_CLOEXEC);
    if (fd<0) goto out;
    ep=epoll_create1(EPOLL_CLOEXEC);
    if (ep<0) goto out;
    event.events=EPOLLIN|EPOLLWAKEUP;
    event.data.fd=fd;
    if (epoll_ctl(ep,EPOLL_CTL_ADD,fd,&event)) goto out;
    spec.it_value.tv_nsec=20000000;
    if (timerfd_settime(fd,0,&spec,NULL)) goto out;
    if (epoll_wait(ep,&event,1,2000)!=1) goto out;
    if (read(fd,&expirations,sizeof(expirations))!=(ssize_t)sizeof(expirations)) goto out;
    if (expirations!=1) goto out;
    failed=0;
out:
    if (failed) fprintf(stderr,"FAIL %s errno=%d (%s)\n",name,errno,strerror(errno));
    else printf("PASS %s create/arm/epoll/read\n",name);
    if (ep>=0) close(ep);
    if (fd>=0) close(fd);
    return failed;
}

int main(void)
{
    struct utsname u;
    struct itimerspec disarmed={0};
    int failures=0, fd, cap;
    struct timespec now;
    if (!uname(&u)) printf("KERNEL_UNDER_TEST %s %s %s\n",u.sysname,u.release,u.machine);
    /* AlarmManager's timerfd path does not use POSIX alarm clock_gettime.
     * Report RTC availability separately: old VExpress PL031 does not mark
     * itself wake-capable, unlike PW3's MAX77696 platform helper. All tests
     * below exercise awake hrtimer firing; none proves RTC wake from suspend. */
    int rtc_ready=clock_gettime(CLOCK_BOOTTIME_ALARM,&now)==0;
    printf("RTC_BACKED_ALARM_CLOCK_AVAILABLE=%d\n",rtc_ready);
    failures+=exercise(CLOCK_REALTIME_ALARM,"CLOCK_REALTIME_ALARM");
    failures+=exercise(CLOCK_REALTIME,"CLOCK_REALTIME");
    failures+=exercise(CLOCK_BOOTTIME_ALARM,"CLOCK_BOOTTIME_ALARM");
    failures+=exercise(CLOCK_BOOTTIME,"CLOCK_BOOTTIME");
    failures+=exercise(CLOCK_MONOTONIC,"CLOCK_MONOTONIC");
    fd=timerfd_create(CLOCK_REALTIME,TFD_CLOEXEC);
    if (fd<0 || timerfd_settime(fd,TFD_TIMER_ABSTIME|TFD_TIMER_CANCEL_ON_SET,&disarmed,NULL)) {
        fprintf(stderr,"FAIL AlarmManager disarmed CANCEL_ON_SET initialization\n");failures++;
    } else puts("PASS AlarmManager disarmed CANCEL_ON_SET initialization");
    if (fd>=0) close(fd);
    cap=prctl(PR_CAPBSET_READ,36,0,0,0);
    if (cap<0) { perror("FAIL CAP_BLOCK_SUSPEND kernel capability range");failures++; }
    else printf("PASS CAP_BLOCK_SUSPEND recognized; current bounding-set bit=%d\n",cap);
    puts("Not tested: actual suspend/resume, RTC wake timing, EPOLLWAKEUP hold lifetime, or realtime clock-change cancellation");
    return failures ? 1 : 0;
}
