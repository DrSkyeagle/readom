/* Exact read-only/exchange helpers copied from the existing parent QEMU suite. */
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
