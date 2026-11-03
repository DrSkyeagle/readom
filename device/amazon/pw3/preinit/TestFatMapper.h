/* SPDX-License-Identifier: Apache-2.0
 * Exact single linear FAT view. Table operations change kernel state only. */
#ifndef PW3_TEST_FAT_MAPPER_H
#define PW3_TEST_FAT_MAPPER_H
#include <linux/dm-ioctl.h>
#include <sys/ioctl.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#define PW3_DM_NAME "pw3-test-fat"
#define PW3_DM_UUID "PW3-TEST-FAT-V1"
union pw3_dm_buffer { uint64_t align; unsigned char bytes[4096]; };
static inline struct dm_ioctl *pw3_dm_request(union pw3_dm_buffer *b) {
    memset(b,0,sizeof(*b));struct dm_ioctl *d=(struct dm_ioctl *)b->bytes;
    d->version[0]=4;d->data_size=sizeof(*b);d->data_start=sizeof(*d);
    strcpy(d->name,PW3_DM_NAME);return d;
}
static inline uint64_t pw3_dm_encode(unsigned major_value,unsigned minor_value) {
    return (minor_value&255U)|((uint64_t)major_value<<8)|((uint64_t)(minor_value&~255U)<<12);
}
static inline bool pw3_testfat_verify(int ctl,unsigned dm_major,unsigned dm_minor,
        unsigned parent_major,unsigned parent_minor,uint64_t offset,uint64_t sectors) {
    union pw3_dm_buffer b;struct dm_ioctl *d=pw3_dm_request(&b);
    if(ioctl(ctl,DM_DEV_STATUS,d)||strcmp(d->name,PW3_DM_NAME)||strcmp(d->uuid,PW3_DM_UUID)||
       d->dev!=pw3_dm_encode(dm_major,dm_minor)||d->target_count!=1||
       !(d->flags&DM_ACTIVE_PRESENT_FLAG)||(d->flags&(DM_SUSPEND_FLAG|DM_READONLY_FLAG|DM_INACTIVE_PRESENT_FLAG|DM_BUFFER_FULL_FLAG)))return false;
    d=pw3_dm_request(&b);d->flags=DM_STATUS_TABLE_FLAG;
    if(ioctl(ctl,DM_TABLE_STATUS,d)||d->target_count!=1||d->data_start<sizeof(*d)||
       d->data_start>sizeof(b)-sizeof(struct dm_target_spec)-64||d->data_size>sizeof(b)||
       d->dev!=pw3_dm_encode(dm_major,dm_minor)||(d->flags&DM_BUFFER_FULL_FLAG))return false;
    struct dm_target_spec *t=(struct dm_target_spec *)(b.bytes+d->data_start);
    char expected[64];snprintf(expected,sizeof(expected),"%u:%u %llu",parent_major,parent_minor,(unsigned long long)offset);
    const char *params=(const char *)(t+1);
    return t->sector_start==0&&t->length==sectors&&t->status==0&&
        !strncmp(t->target_type,"linear",sizeof(t->target_type))&&
        memchr(params,0,64)!=NULL&&!strcmp(params,expected);
}
static inline bool pw3_testfat_create(int ctl,unsigned parent_major,unsigned parent_minor,
        uint64_t offset,uint64_t sectors,unsigned *dm_major,unsigned *dm_minor) {
    if(!sectors||offset!=16)return false;
    union pw3_dm_buffer b;struct dm_ioctl *d=pw3_dm_request(&b);d->name[0]=0;
    if(ioctl(ctl,DM_LIST_DEVICES,d)||(d->flags&DM_BUFFER_FULL_FLAG)||d->data_start<sizeof(*d)||
       d->data_start>sizeof(b)-sizeof(struct dm_name_list))return false;
    const struct dm_name_list *list=(const struct dm_name_list *)(b.bytes+d->data_start);
    if(list->dev)return false;
    d=pw3_dm_request(&b);strcpy(d->uuid,PW3_DM_UUID);
    if(ioctl(ctl,DM_DEV_CREATE,d)||!d->dev)return false;
    *dm_major=(unsigned)((d->dev>>8)&4095U);
    *dm_minor=(unsigned)((d->dev&255U)|((d->dev>>12)&0xfff00U));
    if(*dm_minor!=0)return false;
    d=pw3_dm_request(&b);d->target_count=1;
    struct dm_target_spec *t=(struct dm_target_spec *)(b.bytes+d->data_start);
    t->sector_start=0;t->length=sectors;strcpy(t->target_type,"linear");
    char *params=(char *)(t+1);
    int n=snprintf(params,64,"%u:%u %llu",parent_major,parent_minor,(unsigned long long)offset);
    if(n<=0||n>=64)return false;
    t->next=(sizeof(*t)+(unsigned)n+1+7)&~7U;
    if(ioctl(ctl,DM_TABLE_LOAD,d))return false;
    d=pw3_dm_request(&b);
    if(ioctl(ctl,DM_DEV_SUSPEND,d))return false;
    return pw3_testfat_verify(ctl,*dm_major,*dm_minor,parent_major,parent_minor,offset,sectors);
}
#endif
