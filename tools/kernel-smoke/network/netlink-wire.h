#ifndef PW3_NETLINK_WIRE_H
#define PW3_NETLINK_WIRE_H
#include <stddef.h>
#include <stdint.h>
/* Deliberately small, pinned Linux UAPI subset. No kernel-internal headers. */
#define NW_ALIGN(n) (((n) + 3u) & ~3u)
#define NW_REQUEST 1u
#define NW_ACK 4u
#define NW_DUMP 0x300u
#define NW_EXCL 0x200u
#define NW_CREATE 0x400u
#define NW_ERROR 2u
#define NW_DONE 3u
#define NW_NEWLINK 16u
#define NW_GETLINK 18u
#define NW_NEWADDR 20u
#define NW_NEWROUTE 24u
#define NW_GETROUTE 26u
#define NW_NEWRULE 32u
#define NW_DELRULE 33u
#define NW_GETRULE 34u
#define NW_IFNAME 3u
#define NW_LINKINFO 18u
#define NW_INFO_KIND 1u
#define NW_IFA_ADDRESS 1u
#define NW_IFA_LOCAL 2u
#define NW_IFA_F_NODAD 2u
#define NW_RTA_DST 1u
#define NW_RTA_OIF 4u
#define NW_RTA_PREFSRC 7u
#define NW_RTA_TABLE 15u
#define NW_RTA_UID 25u
#define NW_FRA_PRIORITY 6u
#define NW_FRA_TABLE 15u
#define NW_FRA_UID_RANGE 20u
#define NW_FR_ACT_TO_TBL 1u
#define NW_TABLE_MAIN 254u
#define NW_SCOPE_LINK 253u
#define NW_RT_UNICAST 1u
#define NW_RT_UNREACHABLE 7u
#define NW_PROTO_STATIC 4u
#define NW_CAP 1024u
struct nw_hdr { uint32_t len; uint16_t type, flags; uint32_t seq, pid; };
struct nw_attr { uint16_t len, type; };
struct nw_ifinfo { uint8_t family, pad; uint16_t type; int32_t index; uint32_t flags, change; };
struct nw_ifaddr { uint8_t family, prefix, flags, scope; uint32_t index; };
struct nw_route { uint8_t family, dst_len, src_len, tos, table, protocol, scope, type; uint32_t flags; };
struct nw_rule { uint8_t family, dst_len, src_len, tos, table, reserved1, reserved2, action; uint32_t flags; };
struct nw_uid_range { uint32_t start, end; };
struct nw_message { union { uint64_t aligned; unsigned char data[NW_CAP]; } storage; };
struct nw_attr_view { const void *data; size_t len; };
void *nw_begin(struct nw_message *, uint16_t type, uint16_t flags, size_t payload);
struct nw_hdr *nw_header(struct nw_message *);
int nw_put(struct nw_message *, uint16_t type, const void *, size_t size);
int nw_u32(struct nw_message *, uint16_t type, uint32_t value);
int nw_dummy_link(struct nw_message *, const char *name);
int nw_attrs(const void *, size_t len, struct nw_attr_view *, size_t count);
int nw_read_u32(const struct nw_attr_view *, uint32_t *);
#endif
