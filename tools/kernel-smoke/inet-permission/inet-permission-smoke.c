#define _GNU_SOURCE
#include "netlink-wire.h"
#include "isolation-guard.h"
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* Only the disposable QEMU kernel: create/close sockets, never bind/send IP.
 * Fresh netns is verified before any credential or namespace mutation below. */
#define NETLINK_ROUTE_SMOKE 0
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
    const char *name; gid_t gid, groups[5]; unsigned count;
    int drop_all, drop_raw, effective_raw, inet_ok, raw_ok;
};
/* UID10000/user0's three AMS baseline groups: shared app50000/cache20000/user9997.
 * Ordinary app cases also have the real Zygote empty bounding/effective/permitted sets.
 * Extra service and explicit-cap cases exercise the preserved privileged override. */
static const struct test_case cases[] = {
    {"app-no-internet",10000,{50000,20000,9997},3,1,0,0,0,0},
    {"app-internet",10000,{50000,20000,9997,3003},4,1,0,0,1,0},
    {"no-groups",10000,{0},0,1,0,0,0,0},
    {"inet-effective-gid",3003,{50000,20000,9997},3,1,0,0,1,0},
    {"app-unrelated-group",10000,{50000,20000,9997,3006},4,1,0,0,0,0},
    {"app-raw-without-inet",10000,{50000,20000,9997,3004},4,1,0,0,0,0},
    {"service-admin-only",10000,{3005},1,0,0,0,0,0},
    {"service-raw-only",10000,{3004},1,0,0,0,1,1},
    {"service-raw-bounding-dropped",10000,{3004},1,0,1,0,0,0},
    {"app-inet-and-raw",10000,{50000,20000,9997,3003,3004},5,1,0,0,1,0},
    {"explicit-effective-raw-cap",10000,{0},0,1,0,1,1,1},
};
static void result(int ok,const char *name) {
    printf("inet-permission-smoke %s/%s: %s (errno=%d)\n",case_name,name,ok?"PASS":"FAIL",errno);
    ++checks;if(!ok)++failures;
}
static void timeout_exit(int sig) {
    const char text[]="inet-permission-smoke FAIL: bounded child timeout\n";
    (void)sig;(void)write(STDERR_FILENO,text,sizeof(text)-1);_exit(124);
}
static int read_file(const char *path,char *buf,size_t cap) {
    int fd=open(path,O_RDONLY|O_CLOEXEC);ssize_t n;if(fd<0)return -1;
    n=read(fd,buf,cap-1);close(fd);if(n<0)return -1;buf[n]=0;return (int)n;
}
static int clear_bounding(void) {
    int i;
    for(i=0;i<64;++i) {
        int r=prctl(PR_CAPBSET_READ,i,0,0,0);
        if(r<0)return i>0 && errno==EINVAL ? 0 : -1;
        if(prctl(PR_CAPBSET_DROP,i,0,0,0))return -1;
    }
    errno=EOVERFLOW;return -1;
}
static int verify_bounding(const struct test_case *t) {
    int i;
    for(i=0;i<64;++i) {
        int r=prctl(PR_CAPBSET_READ,i,0,0,0);
        if(r<0)return i>0 && errno==EINVAL;
        if(t->drop_all && r!=0)return 0;
        if(i==CAP_NET_RAW_TEST && r!=!(t->drop_all || t->drop_raw))return 0;
    }
    return 0;
}
static int verify_groups(const struct test_case *t) {
    gid_t actual[16];unsigned i,j;int n=getgroups(16,actual);
    if(n<0 || (unsigned)n!=t->count)return 0;
    for(i=0;i<t->count;++i) {
        for(j=0;j<t->count;++j)if(actual[j]==t->groups[i])break;
        if(j==t->count)return 0;
    }
    return 1;
}
static void check_socket(const char *name,int family,int type,int protocol,int allowed,int rejected_errno) {
    int fd,saved;errno=0;fd=socket(family,type|SOCK_CLOEXEC,protocol);saved=errno;
    result(allowed ? fd>=0 : fd==-1 && saved==rejected_errno,name);
    if(fd>=0)close(fd);
}
static int child_case(const struct test_case *t) {
    struct sg_links links;struct cap_header_test h={CAP_VERSION_3_TEST,0};
    struct cap_data_test caps[2];unsigned cap=t->effective_raw ? 1u<<CAP_NET_RAW_TEST : 0;int rc;
    case_name=t->name;failures=checks=0;signal(SIGALRM,timeout_exit);alarm(20);
    if(unshare(CLONE_NEWNET)) {perror("inet-permission-smoke unshare");return 2;}
    if(read_link_snapshot(&links) || !sg_child_isolated(&parent_links,&links)) {
        fputs("inet-permission-smoke REFUSED: isolated child netns required\n",stderr);return 77;
    }
    result(1,"isolated-netns");
    if((t->drop_all && clear_bounding()) ||
       (t->drop_raw && prctl(PR_CAPBSET_DROP,CAP_NET_RAW_TEST,0,0,0)) ||
       (t->effective_raw && prctl(PR_SET_KEEPCAPS,1,0,0,0)) ||
       setgroups(t->count,t->groups) || setgid(t->gid) || setuid(10000)) {
        perror("inet-permission-smoke credentials");return 2;
    }
    if(t->effective_raw) {
        memset(caps,0,sizeof(caps));caps[0].effective=caps[0].permitted=cap;
        if(syscall(SYS_capset,&h,caps)) {perror("inet-permission-smoke capset");return 2;}
    }
    result(getuid()==10000 && geteuid()==10000 && getgid()==t->gid && getegid()==t->gid,"uid-and-primary-gid");
    result(verify_groups(t),"exact-supplementary-groups");
    memset(caps,0,sizeof(caps));
    result(syscall(SYS_capget,&h,caps)==0 && caps[0].effective==cap && caps[0].permitted==cap &&
        !(caps[1].effective|caps[1].permitted|caps[0].inheritable|caps[1].inheritable),"exact-capability-sets");
    result(verify_bounding(t),"bounding-set");
    check_socket("ipv4-tcp",AF_INET,SOCK_STREAM,0,t->inet_ok,EACCES);
    check_socket("ipv4-udp",AF_INET,SOCK_DGRAM,0,t->inet_ok,EACCES);
    check_socket("ipv4-raw",AF_INET,SOCK_RAW,1,t->raw_ok,t->inet_ok?EPERM:EACCES);
    check_socket("ipv6-tcp",AF_INET6,SOCK_STREAM,0,t->inet_ok,EACCES);
    check_socket("ipv6-udp",AF_INET6,SOCK_DGRAM,0,t->inet_ok,EACCES);
    check_socket("ipv6-raw",AF_INET6,SOCK_RAW,58,t->raw_ok,t->inet_ok?EPERM:EACCES);
    check_socket("unix-stream-unaffected",AF_UNIX,SOCK_STREAM,0,1,0);
    check_socket("unix-dgram-unaffected",AF_UNIX,SOCK_DGRAM,0,1,0);
    check_socket("route-netlink-unaffected",AF_NETLINK,SOCK_RAW,NETLINK_ROUTE_SMOKE,1,0);
    errno=0;rc=setgroups(1,(gid_t[]){3003});result(rc==-1 && errno==EPERM,"cannot-self-grant-inet");
    errno=0;rc=unshare(CLONE_NEWNS);result(rc==-1 && errno==EPERM,"sys-admin-not-granted");
    printf("inet-permission-smoke %s checks=%d failures=%d\n",case_name,checks,failures);return failures?1:0;
}
int main(int argc,char **argv) {
    struct utsname un;struct stat st;struct sg_links after;char cpu[8192];unsigned i;
    int bad=0,status;pid_t child;setvbuf(stdout,NULL,_IONBF,0);
    if(argc!=2 || strcmp(argv[1],"--run-isolated"))return 64;
    if(geteuid() || uname(&un) || !strstr(un.release,"-pw3-core-smoke") || strncmp(un.machine,"arm",3) ||
       read_file("/proc/cpuinfo",cpu,sizeof(cpu))<0 || !strstr(cpu,"ARM-Versatile Express")) {
        fputs("inet-permission-smoke REFUSED: disposable VExpress smoke kernel required\n",stderr);return 77;
    }
    if(stat("/proc/modules",&st)==0 || errno!=ENOENT) {
        fputs("inet-permission-smoke REFUSED: CONFIG_MODULES=n required\n",stderr);return 77;
    }
    if(read_link_snapshot(&parent_links) || !sg_parent_usable(&parent_links)) {
        fputs("inet-permission-smoke REFUSED: parent lo/dummy0 baseline absent\n",stderr);return 77;
    }
    for(i=0;i<sizeof(cases)/sizeof(cases[0]);++i) {
        child=fork();
        if(child<0)return 2;
        if(!child)_exit(child_case(&cases[i]));
        if(waitpid(child,&status,0)!=child)return 2;
        if(!WIFEXITED(status) || WEXITSTATUS(status))bad++;
    }
    if(read_link_snapshot(&after) || !sg_same_membership(&parent_links,&after)) {
        fputs("inet-permission-smoke FAIL: parent membership changed\n",stderr);return 2;
    }
    printf("inet-permission-smoke parent-membership-unchanged: PASS\n");
    printf("inet-permission-smoke cases=%u failed_cases=%d\n",(unsigned)(sizeof(cases)/sizeof(cases[0])),bad);
    return bad?1:0;
}
