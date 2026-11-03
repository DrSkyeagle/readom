#define _GNU_SOURCE
#include "netlink-wire.h"
#include "isolation-guard.h"
#include <arpa/inet.h>
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
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

/* Disposable VExpress QEMU only. No block device, module, firmware, external
 * interface, DNS, payload send, or persistent filesystem write. The only
 * pre-clone observations are platform/mount guards, a read-only RTM_GETLINK
 * snapshot and qtaguid stat. No netns symlink or proc inode identity assumed.
 * Every network mutation follows CLONE_NEWNET and private mount setup.
 */
#define NETLINK_ROUTE_SMOKE 0
#define NETLINK_NETFILTER_SMOKE 12
#define SMOKE_TIMEOUT_SECONDS 60
struct nl_address { unsigned short family, pad; unsigned int pid, groups; };
struct xt_revision { char name[29]; unsigned char revision; };
static unsigned char child_stack[256 * 1024] __attribute__((aligned(16)));
static int failures, checks, route_fd = -1, qtaguid_present;
static uint32_t serial;
static char parent_mount_name[80];
static struct sg_links parent_links;
static unsigned int ifaces[2];
static void result(int ok, const char *name)
{
    int saved = errno;
    if (ok) printf("network-smoke %s: PASS\n", name);
    else printf("network-smoke %s: FAIL (errno=%d)\n", name, saved);
    fflush(stdout); checks++; if (!ok) failures++;
}
static void timeout_exit(int sig)
{
    static const char text[] = "network-smoke FAIL: bounded watchdog expired\n";
    (void)sig; (void)write(STDERR_FILENO, text, sizeof(text) - 1); _exit(124);
}
static int read_file(const char *path, char *buf, size_t cap)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC); ssize_t n;
    if (fd < 0) return -1;
    n = read(fd, buf, cap - 1); close(fd);
    if (n < 0) return -1;
    buf[n] = 0; return (int)n;
}
static int sysctl_value(const char *path, int value, int change)
{
    char b[64], *end; long got; int fd, n; ssize_t written;
    if (change) {
        fd = open(path, O_WRONLY | O_CLOEXEC); if (fd < 0) return -1;
        n = snprintf(b, sizeof(b), "%d\n", value); written = write(fd, b, (size_t)n); close(fd);
        if (written != n) { if (written >= 0) errno = EIO; return -1; }
    }
    if (read_file(path, b, sizeof(b)) < 0) return -1;
    errno = 0; got = strtol(b, &end, 10);
    if (errno || end == b || got != value) { errno = EPROTO; return -1; }
    return 0;
}
static int mount_namespace_name(char *out, size_t n)
{
    /* 0300 implements this symlink only for mnt. The net file intentionally
     * retains the Linux 3.0 regular-file ABI and must not be readlink'd. */
    ssize_t got = readlink("/proc/self/ns/mnt", out, n - 1);
    if (got < 0) return -1;
    out[got] = 0; return 0;
}
static int open_netlink(int protocol)
{
    struct nl_address a; int fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, protocol);
    if (fd < 0) return -1;
    memset(&a, 0, sizeof(a)); a.family = AF_NETLINK;
    if (bind(fd, (struct sockaddr *)&a, sizeof(a))) { int e = errno; close(fd); errno = e; return -1; }
    return fd;
}
typedef int (*message_cb)(const struct nw_hdr *, void *);
static int exchange(struct nw_message *message, int dump, message_cb cb, void *arg)
{
    struct nw_hdr *h = nw_header(message); struct nl_address kernel;
    unsigned int rounds; uint32_t sequence = ++serial;
    memset(&kernel, 0, sizeof(kernel)); kernel.family = AF_NETLINK;
    h->seq = sequence;
    if (sendto(route_fd, h, h->len, 0, (struct sockaddr *)&kernel, sizeof(kernel)) != (ssize_t)h->len) return -1;
    for (rounds = 0; rounds < 128; rounds++) {
        union { uint64_t align; unsigned char data[32768]; } buffer;
        struct pollfd pfd = { route_fd, POLLIN, 0 }; struct iovec iov;
        struct msghdr msg; struct nl_address sender; ssize_t received; size_t at;
        int r;
        do r = poll(&pfd, 1, 3000); while (r < 0 && errno == EINTR);
        if (r <= 0) { if (!r) errno = ETIMEDOUT; return -1; }
        memset(&msg, 0, sizeof(msg)); memset(&sender, 0, sizeof(sender));
        iov.iov_base = buffer.data; iov.iov_len = sizeof(buffer.data);
        msg.msg_iov = &iov; msg.msg_iovlen = 1; msg.msg_name = &sender; msg.msg_namelen = sizeof(sender);
        received = recvmsg(route_fd, &msg, MSG_TRUNC);
        if (received < 0) return -1;
        if ((size_t)received > sizeof(buffer.data) || msg.msg_flags & MSG_TRUNC) { errno = EMSGSIZE; return -1; }
        if (sender.pid != 0) { errno = EPROTO; return -1; }
        for (at = 0; at < (size_t)received;) {
            const struct nw_hdr *reply; size_t step;
            if ((size_t)received - at < sizeof(*reply)) { errno = EPROTO; return -1; }
            reply = (const struct nw_hdr *)(buffer.data + at); step = NW_ALIGN(reply->len);
            if (reply->len < sizeof(*reply) || step > (size_t)received - at) { errno = EPROTO; return -1; }
            at += step;
            if (reply->seq != sequence) continue;
            if (reply->type == NW_ERROR) {
                int32_t error;
                if (reply->len < sizeof(*reply) + sizeof(error)) { errno = EPROTO; return -1; }
                memcpy(&error, (const char *)reply + sizeof(*reply), sizeof(error));
                if (error) { errno = -error; return -1; }
                if (!dump) return 0;
            } else if (reply->type == NW_DONE) {
                /* Linux may include an error integer with NLMSG_DONE. */
                if (reply->len >= sizeof(*reply) + sizeof(int32_t)) {
                    int32_t error; memcpy(&error, (const char *)reply + sizeof(*reply), sizeof(error));
                    if (error) { errno = error < 0 ? -error : error; return -1; }
                }
                return 0;
            } else if (cb && cb(reply, arg)) return -1;
        }
    }
    errno = E2BIG; return -1;
}
static int collect_link(const struct nw_hdr *h, void *arg)
{
    struct sg_links *s = arg; const struct nw_ifinfo *i; struct nw_attr_view a[32]; struct sg_link *link;
    if (h->type != NW_NEWLINK) return 0;
    if (h->len < sizeof(*h) + sizeof(*i) || s->count >= SG_MAX_LINKS) { errno = EOVERFLOW; return -1; }
    i = (const struct nw_ifinfo *)((const char *)h + sizeof(*h));
    if (nw_attrs((const char *)i + sizeof(*i), h->len - sizeof(*h) - sizeof(*i), a, 32)) return -1;
    if (!a[NW_IFNAME].data || !a[NW_IFNAME].len || a[NW_IFNAME].len > SG_IFNAMSIZ ||
        !memchr(a[NW_IFNAME].data, 0, a[NW_IFNAME].len)) { errno = EPROTO; return -1; }
    link = &s->links[s->count++]; link->index = i->index; link->type = i->type; link->flags = i->flags;
    memcpy(link->name, a[NW_IFNAME].data, a[NW_IFNAME].len);
    return 0;
}
static int read_link_snapshot(struct sg_links *s)
{
    struct nw_message m; struct nw_ifinfo *i; int saved_fd = route_fd, rc, error;
    memset(s, 0, sizeof(*s));
    /* Always open a fresh socket in the calling task's current netns. An
     * inherited parent netlink socket would keep reading the initial net. */
    route_fd = open_netlink(NETLINK_ROUTE_SMOKE);
    if (route_fd < 0) { route_fd = saved_fd; return -1; }
    i = nw_begin(&m, NW_GETLINK, NW_REQUEST | NW_DUMP, sizeof(*i));
    rc = i ? exchange(&m, 1, collect_link, s) : -1; error = errno;
    close(route_fd); route_fd = saved_fd; errno = error;
    if (!rc && !sg_valid(s)) { errno = EPROTO; return -1; }
    return rc;
}
static void show_link_snapshot(const char *who, const struct sg_links *s)
{
    unsigned int i;
    printf("network-smoke %s links:", who);
    for (i = 0; i < s->count; i++) printf(" %s(index=%d,type=%u,flags=0x%x)",
        s->links[i].name, (int)s->links[i].index, (unsigned)s->links[i].type, (unsigned)s->links[i].flags);
    putchar('\n');
}
static int create_dummy(const char *name)
{
    struct nw_message m;
    if (nw_dummy_link(&m, name)) return -1;
    return exchange(&m, 0, NULL, NULL);
}
static int set_up(unsigned int index)
{
    struct nw_message m; struct nw_ifinfo *i = nw_begin(&m, NW_NEWLINK, NW_REQUEST | NW_ACK, sizeof(*i));
    if (!i) return -1;
    i->index = (int32_t)index; i->flags = IFF_UP; i->change = IFF_UP;
    return exchange(&m, 0, NULL, NULL);
}
static int add_address(int family, unsigned int index, const char *text, unsigned int prefix)
{
    unsigned char address[16]; size_t bytes = family == AF_INET ? 4 : 16;
    struct nw_message m; struct nw_ifaddr *i = nw_begin(&m, NW_NEWADDR, NW_REQUEST | NW_ACK | NW_CREATE | NW_EXCL, sizeof(*i));
    if (!i || inet_pton(family, text, address) != 1) { errno = EINVAL; return -1; }
    i->family = (uint8_t)family; i->prefix = (uint8_t)prefix; i->index = index;
    i->flags = family == AF_INET6 ? NW_IFA_F_NODAD : 0;
    if (nw_put(&m, NW_IFA_ADDRESS, address, bytes) || (family == AF_INET && nw_put(&m, NW_IFA_LOCAL, address, bytes))) return -1;
    return exchange(&m, 0, NULL, NULL);
}
static int add_route(int family, unsigned int index, const char *dst, unsigned int prefix, const char *source, uint32_t table)
{
    unsigned char address[16], src[16]; size_t bytes = family == AF_INET ? 4 : 16;
    struct nw_message m; struct nw_route *r = nw_begin(&m, NW_NEWROUTE, NW_REQUEST | NW_ACK | NW_CREATE | NW_EXCL, sizeof(*r));
    if (!r || inet_pton(family, dst, address) != 1 || inet_pton(family, source, src) != 1) { errno = EINVAL; return -1; }
    r->family = (uint8_t)family; r->dst_len = (uint8_t)prefix;
    r->protocol = NW_PROTO_STATIC; r->scope = NW_SCOPE_LINK; r->type = NW_RT_UNICAST;
    if (nw_put(&m, NW_RTA_DST, address, bytes) || nw_u32(&m, NW_RTA_OIF, index) ||
        nw_put(&m, NW_RTA_PREFSRC, src, bytes) || nw_u32(&m, NW_RTA_TABLE, table)) return -1;
    return exchange(&m, 0, NULL, NULL);
}
static int rule_op(int family, int add, uint32_t priority, uint32_t table, const void *range, size_t range_len)
{
    struct nw_message m; struct nw_rule *r = nw_begin(&m, add ? NW_NEWRULE : NW_DELRULE,
        NW_REQUEST | NW_ACK | (add ? NW_CREATE | NW_EXCL : 0), sizeof(*r));
    if (!r) return -1;
    r->family = (uint8_t)family; r->action = NW_FR_ACT_TO_TBL;
    if (nw_u32(&m, NW_FRA_PRIORITY, priority) || nw_u32(&m, NW_FRA_TABLE, table) ||
        (range && nw_put(&m, NW_FRA_UID_RANGE, range, range_len))) return -1;
    return exchange(&m, 0, NULL, NULL);
}
struct rule_found { int family, count, has_range; uint32_t priority, table; struct nw_uid_range range; };
static int collect_rule(const struct nw_hdr *h, void *arg)
{
    struct rule_found *f = arg; const struct nw_rule *r; struct nw_attr_view a[32]; uint32_t priority;
    if (h->type != NW_NEWRULE) return 0;
    if (h->len < sizeof(*h) + sizeof(*r)) { errno = EPROTO; return -1; }
    r = (const struct nw_rule *)((const char *)h + sizeof(*h));
    if (r->family != f->family) return 0;
    if (nw_attrs((const char *)r + sizeof(*r), h->len - sizeof(*h) - sizeof(*r), a, 32)) return -1;
    if (nw_read_u32(&a[NW_FRA_PRIORITY], &priority) || priority != f->priority) return 0;
    f->count++; f->table = r->table;
    if (a[NW_FRA_TABLE].data && nw_read_u32(&a[NW_FRA_TABLE], &f->table)) return -1;
    f->has_range = a[NW_FRA_UID_RANGE].data != NULL;
    if (f->has_range) {
        if (a[NW_FRA_UID_RANGE].len != sizeof(f->range)) { errno = EPROTO; return -1; }
        memcpy(&f->range, a[NW_FRA_UID_RANGE].data, sizeof(f->range));
    }
    return 0;
}
static int dump_rule(struct rule_found *f)
{
    struct nw_message m; struct nw_rule *r = nw_begin(&m, NW_GETRULE, NW_REQUEST | NW_DUMP, sizeof(*r));
    if (!r) return -1;
    r->family = (uint8_t)f->family; f->count = 0; f->has_range = 0;
    return exchange(&m, 1, collect_rule, f);
}
struct route_found { int family, count; unsigned int prefix; unsigned char dst[16]; uint32_t table, oif; int filter, route_type; };
static int collect_route(const struct nw_hdr *h, void *arg)
{
    struct route_found *f = arg; const struct nw_route *r; struct nw_attr_view a[32];
    uint32_t table, index; size_t bytes = f->family == AF_INET ? 4 : 16;
    if (h->type != NW_NEWROUTE) return 0;
    if (h->len < sizeof(*h) + sizeof(*r)) { errno = EPROTO; return -1; }
    r = (const struct nw_route *)((const char *)h + sizeof(*h));
    if (r->family != f->family) return 0;
    if (nw_attrs((const char *)r + sizeof(*r), h->len - sizeof(*h) - sizeof(*r), a, 32)) return -1;
    table = r->table;
    if (a[NW_RTA_TABLE].data && nw_read_u32(&a[NW_RTA_TABLE], &table)) return -1;
    index = 0;
    if (a[NW_RTA_OIF].data) { if (nw_read_u32(&a[NW_RTA_OIF], &index)) return -1; }
    else if (r->type != NW_RT_UNREACHABLE) return 0;
    if (f->filter && (r->dst_len != f->prefix || table != f->table || index != f->oif ||
        a[NW_RTA_DST].len != bytes || memcmp(a[NW_RTA_DST].data, f->dst, bytes))) return 0;
    f->count++; if (!f->filter) { f->table = table; f->oif = index; f->route_type = r->type; }
    return 0;
}
static int prefix_in_table(const char *text, unsigned int prefix, unsigned int index, uint32_t table)
{
    struct nw_message m; struct nw_route *r; struct route_found f;
    memset(&f, 0, sizeof(f)); f.family = AF_INET6; f.prefix = prefix; f.oif = index; f.table = table; f.filter = 1;
    if (inet_pton(AF_INET6, text, f.dst) != 1) return 0;
    r = nw_begin(&m, NW_GETROUTE, NW_REQUEST | NW_DUMP, sizeof(*r)); if (!r) return 0;
    r->family = AF_INET6;
    return exchange(&m, 1, collect_route, &f) == 0 && f.count == 1;
}
static int lookup_uid_route(int family, const char *text, uint32_t uid, struct route_found *f)
{
    struct nw_message m; struct nw_route *r; unsigned char dst[16]; size_t bytes = family == AF_INET ? 4 : 16;
    memset(f, 0, sizeof(*f)); f->family = family;
    if (inet_pton(family, text, dst) != 1) { errno = EINVAL; return -1; }
    r = nw_begin(&m, NW_GETROUTE, NW_REQUEST | NW_ACK, sizeof(*r)); if (!r) return -1;
    r->family = (uint8_t)family; r->dst_len = family == AF_INET ? 32 : 128;
    if (nw_put(&m, NW_RTA_DST, dst, bytes) || nw_u32(&m, NW_RTA_UID, uid)) return -1;
    return exchange(&m, 0, collect_route, f);
}
static int route_uid(int family, const char *text, uint32_t uid, unsigned int expected)
{
    struct route_found f;
    return lookup_uid_route(family, text, uid, &f) == 0 && f.count == 1 &&
           f.route_type == NW_RT_UNICAST && f.oif == expected;
}
static int route_unreachable(int family, const char *text, uint32_t uid)
{
    struct route_found f; int rc;
    errno = 0; rc = lookup_uid_route(family, text, uid, &f);
    /* Old IPv6 GETROUTE can return a RTN_UNREACHABLE route, not an ACK error. */
    return (rc == -1 && errno == ENETUNREACH) ||
           (rc == 0 && f.count == 1 && f.route_type == NW_RT_UNREACHABLE);
}
static int socket_route_child(int family, const char *dst, const char *src, uid_t uid)
{
    int fd; unsigned char want[16]; struct sockaddr_storage peer, local; socklen_t n;
    memset(&peer, 0, sizeof(peer)); memset(&local, 0, sizeof(local));
    /* A normal Android app with INTERNET receives AID_INET from zygote. */
    const gid_t inet_group = 3003;
    if (setgroups(1, &inet_group) || setgid(uid) || setuid(uid)) return 10;
    fd = socket(family, SOCK_DGRAM | SOCK_CLOEXEC, 0); if (fd < 0) return 11;
    if (family == AF_INET) {
        struct sockaddr_in *p = (struct sockaddr_in *)&peer;
        p->sin_family = AF_INET; p->sin_port = htons(9); n = sizeof(*p);
        if (inet_pton(family, dst, &p->sin_addr) != 1) return 12;
    } else {
        struct sockaddr_in6 *p = (struct sockaddr_in6 *)&peer;
        p->sin6_family = AF_INET6; p->sin6_port = htons(9); n = sizeof(*p);
        if (inet_pton(family, dst, &p->sin6_addr) != 1) return 12;
    }
    /* UDP connect selects a route/source without sending an application payload. */
    if (connect(fd, (struct sockaddr *)&peer, n)) return 13;
    n = sizeof(local); if (getsockname(fd, (struct sockaddr *)&local, &n)) return 14;
    if (inet_pton(family, src, want) != 1) return 15;
    close(fd);
    return family == AF_INET ? memcmp(&((struct sockaddr_in *)&local)->sin_addr, want, 4) != 0 :
                              memcmp(&((struct sockaddr_in6 *)&local)->sin6_addr, want, 16) != 0;
}
static int socket_route(int family, const char *dst, const char *src, uid_t uid)
{
    int st; pid_t p = fork();
    if (!p) { int rc = socket_route_child(family, dst, src, uid); if (rc) printf("network-smoke socket-child-error uid=%u family=%d rc=%d errno=%d\n", (unsigned)uid, family, rc, errno); _exit(rc); }
    if (p < 0 || waitpid(p, &st, 0) != p) return 0;
    if (!WIFEXITED(st) || WEXITSTATUS(st)) {
        printf("network-smoke socket-child uid=%u family=%d status=%d\n", (unsigned)uid, family, st); return 0;
    }
    return 1;
}
static void revision_check(int family, const char *name, unsigned char revision, int target)
{
    int fd, r, option; socklen_t len; struct xt_revision value; char label[96];
    memset(&value, 0, sizeof(value)); strncpy(value.name, name, sizeof(value.name) - 1); value.revision = revision;
    fd = socket(family, SOCK_RAW | SOCK_CLOEXEC, IPPROTO_RAW);
    option = family == AF_INET ? (target ? 67 : 66) : (target ? 69 : 68);
    len = sizeof(value); r = fd < 0 ? -1 : getsockopt(fd, family == AF_INET ? IPPROTO_IP : IPPROTO_IPV6, option, &value, &len);
    snprintf(label, sizeof(label), "xtables-%s-rev%u-ipv%d", name, revision, family == AF_INET ? 4 : 6);
    result(r >= 0, label); if (fd >= 0) close(fd);
}
static void uid_contracts(int family)
{
    struct nw_uid_range ranges[2] = {{10000,10009},{10010,10019}}, wrong, bad;
    uint32_t long_range[3] = {10000,10009,0}; struct rule_found f; char label[128]; int i, rc, e;
    const char *dst = family == AF_INET ? "203.0.113.7" : "2001:db8:ffff::7";
    const char *prefix = family == AF_INET ? "203.0.113.0" : "2001:db8:ffff::";
    const char *sources[2] = {family == AF_INET ? "192.0.2.1" : "2001:db8:1::1", family == AF_INET ? "198.51.100.1" : "2001:db8:2::1"};
    for (i = 0; i < 2; i++) {
        snprintf(label, sizeof(label), "uid-route-table-%d-ipv%d", i, family == AF_INET ? 4 : 6);
        result(add_route(family, ifaces[i], prefix, family == AF_INET ? 24 : 64, sources[i], 10001u + (unsigned)i) == 0, label);
        snprintf(label, sizeof(label), "uid-rule-add-%d-ipv%d", i, family == AF_INET ? 4 : 6);
        result(rule_op(family, 1, 10000u + (unsigned)i, 10001u + (unsigned)i, &ranges[i], sizeof(ranges[i])) == 0, label);
        memset(&f, 0, sizeof(f)); f.family = family; f.priority = 10000u + (unsigned)i;
        snprintf(label, sizeof(label), "uid-rule-dump-range-table-%d-ipv%d", i, family == AF_INET ? 4 : 6);
        result(dump_rule(&f) == 0 && f.count == 1 && f.has_range && f.table == 10001u + (unsigned)i &&
               f.range.start == ranges[i].start && f.range.end == ranges[i].end, label);
    }
    bad.start = bad.end = 0;
    snprintf(label, sizeof(label), "root-only-rule-add-ipv%d", family == AF_INET ? 4 : 6);
    result(rule_op(family, 1, 9000, 10001, &bad, sizeof(bad)) == 0, label);
    snprintf(label, sizeof(label), "root-only-rule-selects-table-A-ipv%d", family == AF_INET ? 4 : 6);
    result(route_uid(family, dst, 0, ifaces[0]), label);
    snprintf(label, sizeof(label), "uid-outside-range-remains-unreachable-ipv%d", family == AF_INET ? 4 : 6);
    result(route_unreachable(family, dst, 9999), label);
    for (i = 0; i < 2; i++) {
        snprintf(label, sizeof(label), "rta-uid-route-selection-%d-ipv%d", i, family == AF_INET ? 4 : 6);
        result(route_uid(family, dst, ranges[i].start, ifaces[i]) && route_uid(family, dst, ranges[i].end, ifaces[i]), label);
        snprintf(label, sizeof(label), "actual-socket-uid-source-%d-ipv%d", i, family == AF_INET ? 4 : 6);
        result(socket_route(family, dst, sources[i], ranges[i].start), label);
    }
    snprintf(label, sizeof(label), "actual-socket-uid-cache-return-A-ipv%d", family == AF_INET ? 4 : 6);
    result(socket_route(family, dst, sources[0], ranges[0].start), label);
    bad.start = 10009; bad.end = 10000; errno = 0;
    rc = rule_op(family, 1, 12000, 10001, &bad, sizeof(bad)); e = errno;
    snprintf(label, sizeof(label), "uid-rule-reject-reversed-ipv%d", family == AF_INET ? 4 : 6); result(rc == -1 && e == EINVAL, label);
    bad.start = 10000; bad.end = UINT32_MAX; errno = 0;
    rc = rule_op(family, 1, 12001, 10001, &bad, sizeof(bad)); e = errno;
    snprintf(label, sizeof(label), "uid-rule-reject-invalid-uid-ipv%d", family == AF_INET ? 4 : 6); result(rc == -1 && e == EINVAL, label);
    rc = rule_op(family, 1, 12002, 10001, &ranges[0], 4); e = errno;
    snprintf(label, sizeof(label), "uid-rule-reject-short-abi-ipv%d", family == AF_INET ? 4 : 6); result(rc == -1 && e == ERANGE, label);
    rc = rule_op(family, 1, 12003, 10001, long_range, sizeof(long_range)); e = errno;
    snprintf(label, sizeof(label), "uid-rule-reject-long-abi-ipv%d", family == AF_INET ? 4 : 6); result(rc == -1 && e == EINVAL, label);
    bad.start = bad.end = UINT32_MAX;
    rc = rule_op(family, 0, 10000, 10001, &bad, sizeof(bad)); e = errno;
    snprintf(label, sizeof(label), "uid-rule-delete-reject-invalid-range-ipv%d", family == AF_INET ? 4 : 6);
    result(rc == -1 && e == EINVAL, label);
    wrong = ranges[0]; wrong.end++;
    rc = rule_op(family, 0, 10000, 10001, &wrong, sizeof(wrong)); e = errno;
    snprintf(label, sizeof(label), "uid-rule-delete-mismatched-range-ipv%d", family == AF_INET ? 4 : 6); result(rc == -1 && e == ENOENT, label);
    rc = rule_op(family, 0, 10000, 10100, &ranges[0], sizeof(ranges[0])); e = errno;
    snprintf(label, sizeof(label), "uid-rule-delete-mismatched-extended-table-ipv%d", family == AF_INET ? 4 : 6); result(rc == -1 && e == ENOENT, label);
    rc = rule_op(family, 0, 10000, 10001, &ranges[0], sizeof(ranges[0]));
    memset(&f, 0, sizeof(f)); f.family = family; f.priority = 10000;
    snprintf(label, sizeof(label), "uid-rule-delete-exact-and-dump-absent-ipv%d", family == AF_INET ? 4 : 6); result(rc == 0 && dump_rule(&f) == 0 && !f.count, label);
    result(rule_op(family, 1, 14000, 10001, NULL, 0) == 0, family == AF_INET ? "unrestricted-add-ipv4" : "unrestricted-add-ipv6");
    memset(&f, 0, sizeof(f)); f.family = family; f.priority = 14000;
    result(dump_rule(&f) == 0 && f.count == 1 && !f.has_range, family == AF_INET ? "unrestricted-dump-omits-uid-ipv4" : "unrestricted-dump-omits-uid-ipv6");
}
static void ipv6_table_contracts(int default_selector_set)
{
    char path[160];
    snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/smk0/accept_ra_rt_table");
    result(default_selector_set && sysctl_value(path, -1000, 0) == 0, "ipv6-selector-inherits-default-negative-offset");
    result(add_address(AF_INET6, ifaces[0], "2001:db8:1::1", 64) == 0 &&
        prefix_in_table("2001:db8:1::", 64, ifaces[0], 1000u + ifaces[0]), "ipv6-negative-selector-prefix-table");
    snprintf(path, sizeof(path), "/proc/sys/net/ipv6/conf/smk1/accept_ra_rt_table");
    result(sysctl_value(path, 22000, 1) == 0 && add_address(AF_INET6, ifaces[1], "2001:db8:2::1", 64) == 0 &&
        prefix_in_table("2001:db8:2::", 64, ifaces[1], 22000), "ipv6-positive-selector-prefix-table");
    result(sysctl_value(path, 0, 1) == 0 && add_address(AF_INET6, ifaces[1], "2001:db8:3::1", 64) == 0 &&
        prefix_in_table("2001:db8:3::", 64, ifaces[1], NW_TABLE_MAIN), "ipv6-zero-selector-prefix-main-table");
    /* Selector changes affect subsequent routes; prior routes are not migrated. */
    result(prefix_in_table("2001:db8:2::", 64, ifaces[1], 22000), "ipv6-selector-change-does-not-migrate-existing-prefix");
    printf("network-smoke SCOPE: IPv6 tests exercise shared addrconf prefix-table selection via explicit addresses, not received RA packets.\n");
}
static int isolated_main(void *unused)
{
    int fd, default_set, i; char child_mount[80]; struct sg_links child_links;
    const char *names[2] = {"smk0","smk1"}; char label[80];
    (void)unused; signal(SIGALRM, timeout_exit); alarm(SMOKE_TIMEOUT_SECONDS);
    if (getpid() != 1 || mount_namespace_name(child_mount, sizeof(child_mount)) ||
        !strcmp(parent_mount_name, child_mount)) {
        fprintf(stderr, "network-smoke REFUSED: child PID/mount isolation not established before mutations\n"); return 77;
    }
    if (read_link_snapshot(&child_links)) { perror("network-smoke child read-only link dump"); return 2; }
    show_link_snapshot("child-before-mutations", &child_links);
    if (!sg_child_isolated(&parent_links, &child_links)) {
        fprintf(stderr, "network-smoke REFUSED: child must hide parent dummy0 and contain only lo plus optional DOWN sit0\n"); return 77;
    }
    result(1, "legacy-netns-child-clean-and-parent-dummy0-hidden");
    if (getpid() != 1 || mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) ||
        mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "size=4m") ||
        mount("proc", "/proc", "proc", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL)) {
        perror("network-smoke isolated namespace/mount setup"); return 2;
    }
    result(1, "new-pid-mount-net-namespace-and-private-tmpfs");
    result(qtaguid_present, "qtaguid-initial-net-proc-entry-readonly-stat");
    fd = open_netlink(NETLINK_NETFILTER_SMOKE); result(fd >= 0, "nflog-netlink-netfilter-socket"); if (fd >= 0) close(fd);
    for (i = 0; i < 2; i++) {
        int family = i ? AF_INET6 : AF_INET;
        revision_check(family, "owner", 1, 0); revision_check(family, "quota2", 3, 0);
        revision_check(family, "IDLETIMER", 1, 1); revision_check(family, "NFLOG", 0, 1);
    }
    route_fd = open_netlink(NETLINK_ROUTE_SMOKE); result(route_fd >= 0, "rtnetlink-socket"); if (route_fd < 0) return 2;
    default_set = sysctl_value("/proc/sys/net/ipv6/conf/default/accept_ra_rt_table", -1000, 1) == 0;
    result(default_set, "ipv6-selector-default-roundtrip");
    result(sysctl_value("/proc/sys/net/ipv6/conf/default/dad_transmits", 0, 1) == 0, "namespace-only-disable-dad-delay");
    for (i = 0; i < 2; i++) {
        snprintf(label, sizeof(label), "create-dummy-%d", i); result(create_dummy(names[i]) == 0, label);
        ifaces[i] = if_nametoindex(names[i]);
        snprintf(label, sizeof(label), "dummy-ifindex-up-%d", i); result(ifaces[i] && set_up(ifaces[i]) == 0, label);
        if (!ifaces[i]) return 2;
    }
    result(set_up(if_nametoindex("lo")) == 0, "namespace-loopback-up");
    result(add_address(AF_INET, ifaces[0], "192.0.2.1", 24) == 0, "dummy-A-ipv4-address");
    result(add_address(AF_INET, ifaces[1], "198.51.100.1", 24) == 0, "dummy-B-ipv4-address");
    ipv6_table_contracts(default_set);
    uid_contracts(AF_INET); uid_contracts(AF_INET6);
    close(route_fd); alarm(0);
    printf("network-smoke SUMMARY: checks=%d failed=%d; no DNS, external interface or application payload used\n", checks, failures);
    printf("network-smoke SCOPE: syscall/kernel-contract evidence only; not Android netd, Wi-Fi, VPN, TLS or application connectivity.\n");
    return failures ? 1 : 0;
}
int main(int argc, char **argv)
{
    struct utsname u; struct stat st; struct sg_links after_links; char cpu[8192], mount_after[80]; int status; pid_t child;
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 2 || strcmp(argv[1], "--run-isolated")) {
        fprintf(stderr, "Usage: network-smoke --run-isolated (disposable PW3 VExpress smoke initramfs only)\n"); return 64;
    }
    if (geteuid() || uname(&u) || !strstr(u.release, "-pw3-core-smoke") || strncmp(u.machine, "arm", 3) ||
        read_file("/proc/cpuinfo", cpu, sizeof(cpu)) < 0 || !strstr(cpu, "ARM-Versatile Express")) {
        fprintf(stderr, "network-smoke REFUSED: requires root in the disposable VExpress -pw3-core-smoke kernel\n"); return 77;
    }
    /* The parent VExpress config disables MODULES. Revision queries must not
     * trigger a successful global module load outside our net namespace. */
    if (stat("/proc/modules", &st) == 0 || errno != ENOENT) {
        fprintf(stderr, "network-smoke REFUSED: requires CONFIG_MODULES=n\n"); return 77;
    }
    if (mount_namespace_name(parent_mount_name, sizeof(parent_mount_name))) { perror("network-smoke parent mount namespace"); return 2; }
    if (read_link_snapshot(&parent_links)) { perror("network-smoke parent read-only link dump"); return 2; }
    show_link_snapshot("parent-before-clone", &parent_links);
    if (!sg_parent_usable(&parent_links)) {
        fprintf(stderr, "network-smoke REFUSED: initial network must contain lo/dummy0 and no smk0/smk1\n"); return 77;
    }
    qtaguid_present = stat("/proc/net/xt_qtaguid/ctrl", &st) == 0;
    child = clone(isolated_main, child_stack + sizeof(child_stack), CLONE_NEWPID | CLONE_NEWNS | CLONE_NEWNET | SIGCHLD, NULL);
    if (child < 0) { perror("network-smoke clone"); return 2; }
    if (waitpid(child, &status, 0) != child) { perror("network-smoke wait"); return 2; }
    if (mount_namespace_name(mount_after, sizeof(mount_after)) || strcmp(parent_mount_name, mount_after) ||
        read_link_snapshot(&after_links)) { perror("network-smoke parent post-check"); return 2; }
    show_link_snapshot("parent-after-child", &after_links);
    if (!sg_same_membership(&parent_links, &after_links)) {
        fprintf(stderr, "network-smoke FAIL parent interface membership changed\n"); return 2;
    }
    printf("network-smoke parent-mount-and-link-membership-unchanged: PASS\n");
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}
