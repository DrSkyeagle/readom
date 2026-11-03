#ifndef PW3_SMOKE_ISOLATION_GUARD_H
#define PW3_SMOKE_ISOLATION_GUARD_H
#include <stdint.h>
#include <string.h>
/* Read-only link membership checks for Linux 3.0's regular-file netns ABI.
 * These are not proc inode/namespace identity comparisons. CLONE_NEWNET must
 * also succeed. DUMMY is built in and creates dummy0 only in the initial net.
 */
#define SG_MAX_LINKS 16u
#define SG_IFNAMSIZ 16u
#define SG_ARPHRD_ETHER 1u
#define SG_ARPHRD_LOOPBACK 772u
#define SG_ARPHRD_SIT 776u
#define SG_IFF_UP 1u
struct sg_link { int32_t index; uint16_t type; uint32_t flags; char name[SG_IFNAMSIZ]; };
struct sg_links { unsigned int count; struct sg_link links[SG_MAX_LINKS]; };
static inline const struct sg_link *sg_find(const struct sg_links *s, const char *name)
{
    unsigned int i;
    if (s->count > SG_MAX_LINKS) return NULL;
    for (i = 0; i < s->count; i++) if (!strcmp(s->links[i].name, name)) return &s->links[i];
    return NULL;
}
static inline int sg_valid(const struct sg_links *s)
{
    unsigned int i, j;
    if (!s->count || s->count > SG_MAX_LINKS) return 0;
    for (i = 0; i < s->count; i++) {
        if (s->links[i].index <= 0 || !s->links[i].name[0] ||
            !memchr(s->links[i].name, 0, SG_IFNAMSIZ)) return 0;
        for (j = 0; j < i; j++) if (s->links[i].index == s->links[j].index ||
            !strcmp(s->links[i].name, s->links[j].name)) return 0;
    }
    return 1;
}
static inline int sg_parent_usable(const struct sg_links *s)
{
    const struct sg_link *lo, *dummy;
    if (!sg_valid(s)) return 0;
    lo = sg_find(s, "lo"); dummy = sg_find(s, "dummy0");
    return lo && lo->type == SG_ARPHRD_LOOPBACK && dummy && dummy->type == SG_ARPHRD_ETHER &&
           !sg_find(s, "smk0") && !sg_find(s, "smk1");
}
static inline int sg_child_isolated(const struct sg_links *parent, const struct sg_links *child)
{
    const struct sg_link *lo, *sit;
    if (!sg_parent_usable(parent) || !sg_valid(child)) return 0;
    lo = sg_find(child, "lo");
    if (!lo || lo->type != SG_ARPHRD_LOOPBACK) return 0;
    if (child->count == 1) return 1;
    /* CONFIG_IPV6_SIT creates a separate fallback sit0 in every namespace.
     * Permit only that documented DOWN fallback; do not enable or use it. */
    sit = sg_find(child, "sit0");
    return child->count == 2 && sit && sit->type == SG_ARPHRD_SIT && !(sit->flags & SG_IFF_UP);
}
static inline int sg_same_membership(const struct sg_links *a, const struct sg_links *b)
{
    unsigned int i;
    if (!sg_valid(a) || !sg_valid(b) || a->count != b->count) return 0;
    for (i = 0; i < a->count; i++) {
        const struct sg_link *other = sg_find(b, a->links[i].name);
        if (!other || other->index != a->links[i].index || other->type != a->links[i].type) return 0;
    }
    return 1;
}
#endif
