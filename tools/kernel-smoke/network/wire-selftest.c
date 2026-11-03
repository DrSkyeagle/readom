#include "netlink-wire.h"
#include "isolation-guard.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    struct nw_message m; struct nw_rule *r; struct nw_attr_view a[32];
    struct nw_uid_range uid = {10000,10009}, got; uint32_t table = 0;
    unsigned char broken[8] = {3,0,20,0,0,0,0,0};
    assert(sizeof(struct nw_hdr) == 16); assert(sizeof(struct nw_attr) == 4);
    assert(sizeof(struct nw_rule) == 12); assert(sizeof(struct nw_route) == 12);
    assert(sizeof(struct nw_ifinfo) == 16); assert(sizeof(struct nw_ifaddr) == 8);
    assert(sizeof(struct nw_uid_range) == 8);
    assert(NW_FRA_UID_RANGE == 20 && NW_RTA_UID == 25);
    r = nw_begin(&m, NW_NEWRULE, NW_REQUEST | NW_ACK | NW_CREATE | NW_EXCL, sizeof(*r));
    assert(r); r->family = 2; r->action = NW_FR_ACT_TO_TBL;
    assert(nw_u32(&m, NW_FRA_PRIORITY, 10000) == 0);
    assert(nw_u32(&m, NW_FRA_TABLE, 10001) == 0);
    assert(nw_put(&m, NW_FRA_UID_RANGE, &uid, sizeof(uid)) == 0);
    assert(nw_header(&m)->len == 56 && nw_header(&m)->type == 32 && r->table == 0);
    assert(nw_attrs((unsigned char *)r + sizeof(*r), nw_header(&m)->len - sizeof(struct nw_hdr) - sizeof(*r), a, 32) == 0);
    assert(nw_read_u32(&a[NW_FRA_TABLE], &table) == 0 && table == 10001);
    assert(a[NW_FRA_UID_RANGE].len == sizeof(uid)); memcpy(&got, a[NW_FRA_UID_RANGE].data, sizeof(got));
    assert(got.start == 10000 && got.end == 10009);
    assert(nw_put(&m, 1, NULL, NW_CAP) == -1 && errno == EMSGSIZE);
    assert(nw_attrs(broken, sizeof(broken), a, 32) == -1 && errno == EPROTO);
    broken[0] = 8; assert(nw_attrs(broken, 7, a, 32) == -1 && errno == EPROTO);
    broken[0] = 4; assert(nw_attrs(broken, 3, a, 32) == -1 && errno == EPROTO);
    assert(nw_begin(&m, 0, 0, NW_CAP) == NULL && errno == EMSGSIZE);
    {
        const struct nw_attr *linkinfo; struct nw_attr_view inner[4];
        assert(nw_dummy_link(&m, "smk0") == 0);
        assert(nw_header(&m)->len == 60 && nw_header(&m)->type == NW_NEWLINK);
        assert(nw_header(&m)->flags == (NW_REQUEST | NW_ACK | NW_CREATE | NW_EXCL));
        assert(nw_attrs(m.storage.data + 32, nw_header(&m)->len - 32, a, 32) == 0);
        assert(a[NW_IFNAME].len == 5 && !memcmp(a[NW_IFNAME].data, "smk0", 5));
        assert(a[NW_LINKINFO].len == 12);
        linkinfo = (const struct nw_attr *)((const unsigned char *)a[NW_LINKINFO].data - 4);
        /* The legacy pre-parser compares raw rta_type with IFLA_MAX=28.
         * This assertion regresses the actual EINVAL-causing 0x8012 wire value. */
        assert(linkinfo->type == 18 && linkinfo->type <= 28);
        assert((linkinfo->type | 0x8000u) > 28);
        assert(nw_attrs(a[NW_LINKINFO].data, a[NW_LINKINFO].len, inner, 4) == 0);
        assert(inner[NW_INFO_KIND].len == 6 && !memcmp(inner[NW_INFO_KIND].data, "dummy", 6));
    }
    {
        struct sg_links parent = {2, {{1,SG_ARPHRD_LOOPBACK,0,"lo"},{2,SG_ARPHRD_ETHER,0,"dummy0"}}};
        struct sg_links child = {1, {{1,SG_ARPHRD_LOOPBACK,0,"lo"}}};
        struct sg_links reordered = {2, {{2,SG_ARPHRD_ETHER,0,"dummy0"},{1,SG_ARPHRD_LOOPBACK,0,"lo"}}};
        assert(sg_parent_usable(&parent)); assert(sg_child_isolated(&parent, &child));
        assert(!sg_child_isolated(&parent, &parent)); /* inherited initial net */
        assert(sg_same_membership(&parent, &reordered));
        child.count = 2; child.links[1] = (struct sg_link){2,SG_ARPHRD_SIT,0,"sit0"};
        assert(sg_child_isolated(&parent, &child)); /* per-net fallback, not inherited dummy */
        child.links[1].flags = SG_IFF_UP; assert(!sg_child_isolated(&parent, &child));
        child.links[1] = (struct sg_link){2,SG_ARPHRD_ETHER,0,"eth0"}; assert(!sg_child_isolated(&parent, &child));
        child.links[1] = (struct sg_link){2,SG_ARPHRD_ETHER,0,"dummy0"}; assert(!sg_child_isolated(&parent, &child));
        child.links[1] = (struct sg_link){2,SG_ARPHRD_SIT,0,"smk0"}; assert(!sg_child_isolated(&parent, &child));
        parent.links[1].name[0] = 0; assert(!sg_parent_usable(&parent));
        assert(!sg_same_membership(&parent, &reordered));
    }
    puts("network-wire-selftest: PASS (wire layout/bounds and legacy link guard policy only; no kernel isolation syscalls tested)");
    return 0;
}
