#define _GNU_SOURCE
#include "netlink-wire.h"
#include "isolation-guard.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <net/if.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* Run only inside the disposable -pw3-core-smoke VExpress initramfs.
 * No wireless interface, external traffic, module load or persistent writes.
 * All mutation follows a verified fresh CLONE_NEWNET namespace. */
#define NETLINK_ROUTE_SMOKE 0
#define CAP_NET_ADMIN_TEST 12
#define CAP_NET_RAW_TEST 13
#define CAP_VERSION_3_TEST 0x20080522u
struct nl_address { unsigned short family, pad; unsigned int pid, groups; };
struct cap_header_test { unsigned int version; int pid; };
struct cap_data_test { unsigned int effective, permitted, inheritable; };
static int route_fd = -1, failures, checks;
static uint32_t serial;
static struct sg_links parent_links;
static const char *case_name;
#include "network-helpers.inc"
struct test_case {
    const char *name;
    gid_t gid, groups[2]; unsigned int count;
    int drop_raw, drop_admin, raw_ok, admin_ok;
};
static const struct test_case cases[] = {
    {"no-groups",1010,{0,0},0,0,0,0,0},
    {"inet-only",1010,{3003,0},1,0,0,0,0},
    {"raw-only",1010,{3004,0},1,0,0,1,0},
    {"admin-only",1010,{3005,0},1,0,0,0,1},
    {"both-groups",1010,{3004,3005},2,0,0,1,1},
    {"raw-bounding-dropped",1010,{3004,0},1,1,0,0,0},
    {"admin-bounding-dropped",1010,{3005,0},1,0,1,0,0},
    {"admin-primary-gid",3005,{0,0},0,0,0,0,1},
};
static void result(int ok, const char *name) {
    printf("network-gid-smoke %s/%s: %s (errno=%d)\n",case_name,name,ok?"PASS":"FAIL",errno);
    ++checks; if (!ok) ++failures;
}
static void timeout_exit(int sig) {
    const char text[]="network-gid-smoke FAIL: bounded child timeout\n";
    (void)sig; (void)write(STDERR_FILENO,text,sizeof(text)-1);_exit(124);
}
static int read_file(const char *path,char *buf,size_t cap) {
    int fd=open(path,O_RDONLY|O_CLOEXEC);ssize_t n;if(fd<0)return -1;
    n=read(fd,buf,cap-1);close(fd);if(n<0)return -1;buf[n]=0;return (int)n;
}
static int expected_result(int rc,int allowed) {
    return allowed ? rc>=0 : rc<0 && (errno==EPERM || errno==EACCES);
}
static int child_case(const struct test_case *t) {
    struct sg_links links; struct cap_header_test h={CAP_VERSION_3_TEST,0};
    struct cap_data_test caps[2]; struct ifreq req; struct nw_message m;
    int fd,rc,control_fd;
    case_name=t->name;failures=checks=0;
    signal(SIGALRM,timeout_exit);alarm(20);
    if(unshare(CLONE_NEWNET)) {perror("network-gid-smoke unshare");return 2;}
    if(read_link_snapshot(&links) || !sg_child_isolated(&parent_links,&links)) {
        fprintf(stderr,"network-gid-smoke REFUSED: child netns not isolated\n");return 77;
    }
    result(1,"isolated-netns");
    /* Keep ioctl authorization independent of 0600's IP socket creation gate.
     * This descriptor belongs to the verified private netns; every privileged
     * ioctl is still performed only AFTER UID/group/capability changes. */
    control_fd=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);
    if(control_fd<0)return 2;
    if((t->drop_raw && prctl(PR_CAPBSET_DROP,CAP_NET_RAW_TEST,0,0,0)) ||
       (t->drop_admin && prctl(PR_CAPBSET_DROP,CAP_NET_ADMIN_TEST,0,0,0)) ||
       setgroups(t->count,t->groups) || setgid(t->gid) || setuid(1010)) {
        perror("network-gid-smoke credentials");return 2;
    }
    memset(caps,0,sizeof(caps));
    result(getuid()==1010 && syscall(SYS_capget,&h,caps)==0 &&
        !(caps[0].effective|caps[1].effective|caps[0].permitted|caps[1].permitted),"uid1010-empty-effective-and-permitted-caps");
    result(prctl(PR_CAPBSET_READ,CAP_NET_RAW_TEST,0,0,0)==!t->drop_raw &&
        prctl(PR_CAPBSET_READ,CAP_NET_ADMIN_TEST,0,0,0)==!t->drop_admin,"bounding-set");
    errno=0;fd=socket(AF_PACKET,SOCK_RAW|SOCK_CLOEXEC,0);
    result(expected_result(fd,t->raw_ok),"raw-socket");if(fd>=0)close(fd);
    memset(&req,0,sizeof(req));strcpy(req.ifr_name,"lo");
    if(ioctl(control_fd,SIOCGIFFLAGS,&req)) {close(control_fd);return 2;}
    req.ifr_flags|=IFF_UP;errno=0;rc=ioctl(control_fd,SIOCSIFFLAGS,&req);
    result(expected_result(rc,t->admin_ok),"ioctl-loopback-admin");close(control_fd);
    route_fd=open_netlink(NETLINK_ROUTE_SMOKE);if(route_fd<0)return 2;
    if(nw_dummy_link(&m,"gidtest0"))return 2;
    errno=0;rc=exchange(&m,0,NULL,NULL);
    result(expected_result(rc,t->admin_ok),"netlink-create-dummy");
    result((if_nametoindex("gidtest0")!=0)==t->admin_ok,"dummy-existence");
    close(route_fd);route_fd=-1;
    errno=0;rc=unshare(CLONE_NEWNS);
    result(rc==-1 && errno==EPERM,"sys-admin-not-granted");
    printf("network-gid-smoke %s checks=%d failures=%d\n",case_name,checks,failures);
    return failures ? 1 : 0;
}
int main(int argc,char **argv) {
    struct utsname un;struct stat st;struct sg_links after;char cpu[8192];unsigned i;
    int bad=0,status;pid_t child;
    setvbuf(stdout,NULL,_IONBF,0);
    if(argc!=2 || strcmp(argv[1],"--run-isolated"))return 64;
    if(geteuid() || uname(&un) || !strstr(un.release,"-pw3-core-smoke") || strncmp(un.machine,"arm",3) ||
        read_file("/proc/cpuinfo",cpu,sizeof(cpu))<0 || !strstr(cpu,"ARM-Versatile Express")) {
        fputs("network-gid-smoke REFUSED: disposable VExpress smoke kernel required\n",stderr);return 77;
    }
    if(stat("/proc/modules",&st)==0 || errno!=ENOENT) {
        fputs("network-gid-smoke REFUSED: CONFIG_MODULES=n required\n",stderr);return 77;
    }
    if(read_link_snapshot(&parent_links) || !sg_parent_usable(&parent_links)) {
        fputs("network-gid-smoke REFUSED: parent lo/dummy0 isolation baseline absent\n",stderr);return 77;
    }
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        child=fork();if(child<0)return 2;
        if(!child)_exit(child_case(&cases[i]));
        if(waitpid(child,&status,0)!=child)return 2;
        if(!WIFEXITED(status) || WEXITSTATUS(status))bad++;
    }
    if(read_link_snapshot(&after) || !sg_same_membership(&parent_links,&after)) {
        fputs("network-gid-smoke FAIL: parent membership changed\n",stderr);return 2;
    }
    printf("network-gid-smoke parent-membership-unchanged: PASS\n");
    printf("network-gid-smoke cases=8 failed_cases=%d\n",bad);return bad ? 1 : 0;
}
