#include "netlink-wire.h"
#include <errno.h>
#include <string.h>
struct nw_hdr *nw_header(struct nw_message *m) { return (struct nw_hdr *)m->storage.data; }
void *nw_begin(struct nw_message *m, uint16_t type, uint16_t flags, size_t payload)
{
    struct nw_hdr *h;
    if (payload > NW_CAP - sizeof(*h)) { errno = EMSGSIZE; return NULL; }
    memset(m, 0, sizeof(*m)); h = nw_header(m);
    h->len = (uint32_t)(sizeof(*h) + payload); h->type = type; h->flags = flags;
    return m->storage.data + sizeof(*h);
}
int nw_put(struct nw_message *m, uint16_t type, const void *data, size_t size)
{
    struct nw_hdr *h = nw_header(m); struct nw_attr *a; size_t at, span;
    if (size > UINT16_MAX - sizeof(*a) || h->len < sizeof(*h) || h->len > NW_CAP) { errno = EMSGSIZE; return -1; }
    at = NW_ALIGN(h->len); span = NW_ALIGN(sizeof(*a) + size);
    if (at > NW_CAP || span > NW_CAP - at) { errno = EMSGSIZE; return -1; }
    a = (struct nw_attr *)(m->storage.data + at); a->type = type; a->len = (uint16_t)(sizeof(*a) + size);
    if (size) memcpy((unsigned char *)a + sizeof(*a), data, size);
    memset((unsigned char *)a + sizeof(*a) + size, 0, span - sizeof(*a) - size);
    h->len = (uint32_t)(at + span); return 0;
}
int nw_u32(struct nw_message *m, uint16_t type, uint32_t value) { return nw_put(m, type, &value, sizeof(value)); }
int nw_dummy_link(struct nw_message *m, const char *name)
{
    struct nw_message nested;
    if (!nw_begin(m, NW_NEWLINK, NW_REQUEST | NW_ACK | NW_CREATE | NW_EXCL,
                  sizeof(struct nw_ifinfo)) || !nw_begin(&nested, 0, 0, 0)) return -1;
    /* Linux 3.0 rtnetlink_rcv_msg checks raw rta_type before nla_parse:
     * NLA_F_NESTED on this top-level attribute is rejected with EINVAL.
     * Nesting is implied by ifla_policy[IFLA_LINKINFO] = NLA_NESTED. */
    if (nw_put(&nested, NW_INFO_KIND, "dummy", 6) ||
        nw_put(m, NW_IFNAME, name, strlen(name) + 1) ||
        nw_put(m, NW_LINKINFO, nested.storage.data + sizeof(struct nw_hdr),
               nw_header(&nested)->len - sizeof(struct nw_hdr))) return -1;
    return 0;
}
int nw_attrs(const void *data, size_t len, struct nw_attr_view *out, size_t count)
{
    const unsigned char *p = data;
    memset(out, 0, count * sizeof(*out));
    while (len) {
        struct nw_attr a; size_t span; uint16_t type;
        if (len < sizeof(a)) { errno = EPROTO; return -1; }
        memcpy(&a, p, sizeof(a)); span = NW_ALIGN(a.len); type = a.type & 0x3fffu;
        if (a.len < sizeof(a) || span > len) { errno = EPROTO; return -1; }
        if (type < count) { out[type].data = p + sizeof(a); out[type].len = a.len - sizeof(a); }
        p += span; len -= span;
    }
    return 0;
}
int nw_read_u32(const struct nw_attr_view *v, uint32_t *out)
{
    if (!v->data || v->len != sizeof(*out)) { errno = EPROTO; return -1; }
    memcpy(out, v->data, sizeof(*out)); return 0;
}
