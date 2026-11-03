/* Tests the production mapping reader and response parser at their syscall seam. */
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
static int mock_ioctl(int, unsigned long, ...);
static int mock_fstat(int, struct stat *);
static int mock_fstatfs(int, struct statfs *);
static int mock_fstatvfs(int, struct statvfs *);
#define ioctl mock_ioctl
#define fstat mock_fstat
#define fstatfs mock_fstatfs
#define fstatvfs mock_fstatvfs
#include "../../../device/amazon/pw3/preinit/TestFileMapper.h"
#undef ioctl
#undef fstat
#undef fstatfs
#undef fstatvfs
static unsigned checks, desired=241;
static int fault, reply_fault;
/* Which file the device-mapper reply claims to be about: its name, its uuid
 * and whether it is read-only all come from the kind (TestFileMapper.h). */
static enum pw3_file_kind reply_kind=PW3_FILE_SYSTEM;
static struct pw3_file_mapping reply_map;
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL line=%d %s fault=%d reply=%d\n",__LINE__,#x,fault,reply_fault);exit(1);}}while(0)
/* Image sizes are configured (TestFileFormat.h); these are V37's. */
static uint64_t system_bytes=576ULL<<20,data_bytes=2048ULL<<20;
/* The report area is the one file of fixed size (ReportArea.h); the variable
 * exists so that a size it may not have can be read faithfully and refused. */
static uint64_t report_bytes=PW3_TEST_REPORT_BYTES;
static uint64_t bytes(int fd){return fd==11?system_bytes:fd==13?report_bytes:data_bytes;}
static int mock_fstat(int fd,struct stat *s){
 memset(s,0,sizeof(*s));s->st_mode=S_IFREG|0400;s->st_nlink=1;s->st_size=bytes(fd);s->st_dev=7;s->st_ino=fd;
 if(fault==1){errno=EIO;return -1;}if(fault==2)s->st_nlink=2;if(fault==3)s->st_size--;return 0;
}
/* FAT reports its cluster size as the statfs block size; the user store's is 8 KiB. */
static long cluster_bytes=8192;
static int mock_fstatfs(int fd,struct statfs *s){(void)fd;memset(s,0,sizeof(*s));s->f_type=fault==4?0x1234:0x4d44;
 s->f_bsize=fault==12?12288:fault==13?131072:cluster_bytes;return 0;}
static int mock_fstatvfs(int fd,struct statvfs *s){(void)fd;memset(s,0,sizeof(*s));s->f_flag=fault==5?0:ST_RDONLY;return 0;}
static int mock_ioctl(int fd,unsigned long request,...){
 va_list ap;va_start(ap,request);void *arg=va_arg(ap,void*);va_end(ap);
 if(request==FIGETBSZ){*(int*)arg=fault==6?4096:512;return 0;}
 if(request==FIBMAP){
  /* Runs of whole clusters with a 16-sector gap after each: about `desired`
   * extents. The mapper queries the first and last sector of each cluster. */
  int n=*(int*)arg;if(fault==7&&n==16){errno=EIO;return -1;}
  uint64_t spc=(uint64_t)cluster_bytes/512,run=(bytes(fd)/512)/desired/spc*spc;if(!run)run=spc;
  /* Three physical ranges that cannot run into each other at any size these
   * files may have: the data image at its largest reaches about 10.3 M. */
  int value=(fd==11?1000:fd==13?12000000:4000000)+n+(n/(int)run)*16;
  if(fault==8&&n==16)value=0;
  if(fault==9&&n==16)value=16000000;
  if(fault==10&&n>=32&&n<48)value-=16; /* the third cluster where the second is */
  if(fault==11&&n==31)value++;          /* a cluster whose ends do not span it */
  *(int*)arg=value;return 0;
 }
 struct dm_ioctl *d=arg;
 assert(request==DM_DEV_STATUS||request==DM_TABLE_STATUS);
 d->dev=pw3_dm_encode(253,1);d->target_count=reply_map.count;
 d->flags=DM_ACTIVE_PRESENT_FLAG|(pw3_file_map_readonly(reply_kind)?DM_READONLY_FLAG:0);
 strcpy(d->name,pw3_file_map_name(reply_kind));strcpy(d->uuid,pw3_file_map_uuid(reply_kind));
 if(reply_fault==1)d->name[0]='X';
 if(reply_fault==2)d->uuid[0]='X';
 if(reply_fault==3)d->dev++;
 if(reply_fault==4)d->target_count++;
 /* Write access is the kind's, so the fault is the flag the other way round:
  * read-only on a writable map is as wrong as writable on the system image. */
 if(reply_fault==5)d->flags^=DM_READONLY_FLAG;
 if(reply_fault==6)d->flags&=~DM_ACTIVE_PRESENT_FLAG;
 if(reply_fault==7)d->flags|=DM_BUFFER_FULL_FLAG;
 if(reply_fault==8)d->flags|=DM_SUSPEND_FLAG;
 if(reply_fault==9)d->flags|=DM_INACTIVE_PRESENT_FLAG;
 if(request==DM_DEV_STATUS)return 0;
 d->data_start=sizeof(*d);uint32_t cursor=0;size_t used=0;
 for(unsigned i=0;i<reply_map.count;i++){
  struct dm_target_spec *t=(void*)((char*)d+d->data_start+cursor);
  t->sector_start=reply_map.extents[i].logical;t->length=reply_map.extents[i].sectors;
  strcpy(t->target_type,"linear");char *p=(char*)(t+1);
  int n=sprintf(p,"179:4 %llu",(unsigned long long)reply_map.extents[i].start);
  used=cursor+sizeof(*t)+(unsigned)n+1;t->next=(used+7)&~7U;cursor=t->next;
 }
 d->data_size=d->data_start+used;
 struct dm_target_spec *first=(void*)((char*)d+d->data_start);
 struct dm_target_spec *second=(void*)((char*)first+first->next);
 switch(reply_fault){
 case 10:second->sector_start++;break;case 11:second->length--;break;
 case 12:second->status=1;break;case 13:strcpy(second->target_type,"zero");break;
 case 14:((char*)(second+1))[0]='X';break;case 15:first->next=0;break;
 case 16:first->next=UINT32_MAX;break;case 17:first->next++;break;
 case 18:d->data_start=UINT32_MAX;break;case 19:d->data_start++;break;
 case 20:d->data_size=sizeof(*d)-1;break;case 21:d->data_size=PW3_FILE_DM_BYTES+1;break;
 case 22:d->data_size--;break;case 23:d->data_size++;break;
 case 24:memset(first+1,'X',64);break;case 25:second->next-=first->next;break;
 case 26:d->dev++;break;case 27:d->uuid[0]='X';break;
 }
 return 0;
}
int main(void){
 struct pw3_file_maps maps;memset(&maps,0,sizeof(maps));maps.magic=PW3_FILEMAP_MAGIC;
 desired=1;CHECK(pw3_file_mapping_read(11,system_bytes,16000000,0,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_OK);
 CHECK(maps.map[PW3_FILE_SYSTEM].count==1);
 /* Nothing may be mapped before the FAT's data area: a chain whose last hop is
  * a cluster below two answers with a sector inside the FAT tables, and the
  * driver does not refuse it (fs/fat/cache.c, fat_get_cluster). The stub's
  * first sector here is 1000. */
 CHECK(pw3_file_mapping_read(11,system_bytes,16000000,1001,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_HOLE_OR_BOUNDS);
 CHECK(pw3_file_mapping_read(11,system_bytes,16000000,1000,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_OK);
 desired=1024;CHECK(pw3_file_mapping_read(11,system_bytes,16000000,0,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_OK);
 CHECK(maps.map[PW3_FILE_SYSTEM].count==1024);
 CHECK(pw3_file_mapping_read(12,data_bytes,16000000,0,&maps.map[PW3_FILE_DATA])==PW3_FILEMAP_OK);
 CHECK(maps.map[PW3_FILE_DATA].count==1024);
 /* A receipt with no report area is what an installation from before there was
  * one leaves, and what the installer leaves when the store has no room or the
  * name is taken by somebody's file. It is accepted; a missing image is not. */
 CHECK(pw3_file_maps_bounds(&maps,16000016));
 CHECK(!pw3_file_maps_present(&maps,PW3_FILE_REPORT));
 CHECK(pw3_file_maps_present(&maps,PW3_FILE_SYSTEM)&&pw3_file_maps_present(&maps,PW3_FILE_DATA));
 desired=2;CHECK(pw3_file_mapping_read(13,report_bytes,16000000,0,&maps.map[PW3_FILE_REPORT])==PW3_FILEMAP_OK);
 CHECK(maps.map[PW3_FILE_REPORT].count==2&&maps.map[PW3_FILE_REPORT].sectors==PW3_TEST_REPORT_BYTES/512);
 CHECK(pw3_file_maps_present(&maps,PW3_FILE_REPORT)&&pw3_file_maps_bounds(&maps,16000016));
 struct pw3_file_maps *wrong=malloc(sizeof(*wrong));CHECK(wrong!=NULL);
 #define BAD(change) do{*wrong=maps;change;CHECK(!pw3_file_maps_bounds(wrong,16000016));}while(0)
 BAD(wrong->magic--);BAD(wrong->map[PW3_FILE_DATA].count=0);BAD(wrong->map[PW3_FILE_DATA].count=1025);
 BAD(wrong->map[PW3_FILE_DATA].reserved=1);BAD(wrong->map[PW3_FILE_DATA].sectors--);BAD(wrong->map[PW3_FILE_DATA].device++);
 BAD(wrong->map[PW3_FILE_DATA].inode=wrong->map[PW3_FILE_SYSTEM].inode);BAD(wrong->map[PW3_FILE_SYSTEM].inode=0);
 BAD(wrong->map[PW3_FILE_DATA].extents[0].logical=1);BAD(wrong->map[PW3_FILE_DATA].extents[1].logical--);
 BAD(wrong->map[PW3_FILE_DATA].extents[2].logical++);BAD(wrong->map[PW3_FILE_DATA].extents[0].sectors=0);
 BAD(wrong->map[PW3_FILE_DATA].extents[0].sectors=UINT64_MAX);BAD(wrong->map[PW3_FILE_DATA].extents[0].start=15);
 BAD(wrong->map[PW3_FILE_DATA].extents[0].start=16000016);BAD(wrong->map[PW3_FILE_DATA].extents[0].start=UINT64_MAX-1);
 BAD(wrong->map[PW3_FILE_DATA].extents[1].start=wrong->map[PW3_FILE_DATA].extents[0].start);
 BAD(wrong->map[PW3_FILE_DATA].extents[0].start=wrong->map[PW3_FILE_SYSTEM].extents[0].start);
 /* The report area is optional, not exempt: present, it is held to everything
  * an image is -- its own size, its own inode, its own clusters. */
 BAD(wrong->map[PW3_FILE_REPORT].sectors--);
 BAD(wrong->map[PW3_FILE_REPORT].sectors=PW3_TEST_REPORT_BYTES/512+8);
 BAD(wrong->map[PW3_FILE_REPORT].inode=wrong->map[PW3_FILE_DATA].inode);
 BAD(wrong->map[PW3_FILE_REPORT].device++);
 BAD(wrong->map[PW3_FILE_REPORT].extents[0].start=wrong->map[PW3_FILE_DATA].extents[0].start);
 BAD(wrong->map[PW3_FILE_REPORT].extents[0].start=wrong->map[PW3_FILE_SYSTEM].extents[3].start);
 BAD(wrong->map[PW3_FILE_REPORT].count=0);
 BAD(wrong->map[PW3_FILE_REPORT].extents[0].logical=1);
 free(wrong);
 /* A report area of any length but 64 KiB is read faithfully and refused: it is
  * written by sector through clusters mapped for exactly that many. */
 {struct pw3_file_maps third=maps;
  report_bytes=PW3_TEST_REPORT_BYTES+4096;desired=2;
  CHECK(pw3_file_mapping_read(13,report_bytes,16000000,0,&third.map[PW3_FILE_REPORT])==PW3_FILEMAP_OK);
  CHECK(!pw3_file_maps_bounds(&third,16000016));
  report_bytes=PW3_TEST_REPORT_BYTES;
  CHECK(pw3_file_mapping_read(13,report_bytes,16000000,0,&third.map[PW3_FILE_REPORT])==PW3_FILEMAP_OK);
  CHECK(pw3_file_maps_bounds(&third,16000016));}
 /* Sizes outside the limits of each image, or not whole 4 KiB blocks, are
  * read faithfully but not admitted. */
 struct pw3_file_maps sized=maps;
 #define SIZED(sys,dat,ok) do{system_bytes=(sys);data_bytes=(dat);desired=3;\
  CHECK(pw3_file_mapping_read(11,system_bytes,16000000,0,&sized.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_OK);\
  CHECK(pw3_file_mapping_read(12,data_bytes,16000000,0,&sized.map[PW3_FILE_DATA])==PW3_FILEMAP_OK);\
  CHECK(pw3_file_maps_bounds(&sized,16000016)==(ok));}while(0)
 SIZED(PW3_TEST_SYSTEM_MAX_BYTES,PW3_TEST_DATA_MAX_BYTES,true);
 SIZED(PW3_TEST_SYSTEM_MIN_BYTES,PW3_TEST_DATA_MIN_BYTES,true);
 SIZED(PW3_TEST_SYSTEM_MAX_BYTES+4096,2048ULL<<20,false);
 SIZED(PW3_TEST_SYSTEM_MIN_BYTES-4096,2048ULL<<20,false);
 SIZED(576ULL<<20,PW3_TEST_DATA_MAX_BYTES+4096,false);
 SIZED(576ULL<<20,PW3_TEST_DATA_MIN_BYTES-4096,false);
 SIZED((576ULL<<20)+512,2048ULL<<20,false);
 SIZED(576ULL<<20,(2048ULL<<20)-512,false);
 system_bytes=576ULL<<20;data_bytes=2048ULL<<20;
 desired=1025;CHECK(pw3_file_mapping_read(11,system_bytes,16000000,0,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_EXTENT_LIMIT);
 desired=1;CHECK(pw3_file_mapping_read(11,system_bytes,16000000,0,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_OK);
 maps.map[PW3_FILE_SYSTEM].extents[1].start=99;CHECK(!pw3_file_mapping_bounds(&maps.map[PW3_FILE_SYSTEM],system_bytes,16000016));
 static const enum pw3_file_map_error expected[]={PW3_FILEMAP_OK,PW3_FILEMAP_METADATA,PW3_FILEMAP_METADATA,PW3_FILEMAP_METADATA,PW3_FILEMAP_FILESYSTEM,PW3_FILEMAP_READONLY,PW3_FILEMAP_BLOCK_SIZE,PW3_FILEMAP_QUERY,PW3_FILEMAP_HOLE_OR_BOUNDS,PW3_FILEMAP_HOLE_OR_BOUNDS,PW3_FILEMAP_OVERLAP,PW3_FILEMAP_CLUSTER,PW3_FILEMAP_CLUSTER,PW3_FILEMAP_CLUSTER};
 for(fault=1;fault<=13;fault++)CHECK(pw3_file_mapping_read(11,system_bytes,16000000,0,&maps.map[PW3_FILE_SYSTEM])==expected[fault]);
 fault=0;
 /* A last cluster partly beyond the end of the file (4 KiB of an 8 KiB cluster). */
 system_bytes=(576ULL<<20)+4096;desired=5;
 CHECK(pw3_file_mapping_read(11,system_bytes,16000000,0,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_OK);
 CHECK(maps.map[PW3_FILE_SYSTEM].sectors==system_bytes/512&&pw3_file_mapping_bounds(&maps.map[PW3_FILE_SYSTEM],system_bytes,16000016));
 system_bytes=576ULL<<20;
 /* Other cluster sizes: 512 B (one sector) and 64 KiB. */
 for(long c=512;c<=65536;c*=128){cluster_bytes=c;desired=241;
  CHECK(pw3_file_mapping_read(12,data_bytes,16000000,0,&maps.map[PW3_FILE_DATA])==PW3_FILEMAP_OK);
  CHECK(pw3_file_mapping_bounds(&maps.map[PW3_FILE_DATA],data_bytes,16000016));}
 cluster_bytes=8192;
 CHECK(pw3_file_mapping_read(11,513,16000000,0,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_INPUT);
 CHECK(pw3_file_mapping_read(11,system_bytes,UINT64_MAX,0,&maps.map[PW3_FILE_SYSTEM])==PW3_FILEMAP_INPUT);
 memset(&reply_map,0,sizeof(reply_map));reply_map.device=7;reply_map.inode=11;
 reply_map.sectors=system_bytes/512;reply_map.count=3;
 reply_map.extents[0]=(struct pw3_file_extent){0,100,2};
 reply_map.extents[1]=(struct pw3_file_extent){2,200,3};
 reply_map.extents[2]=(struct pw3_file_extent){5,1000,reply_map.sectors-5};
 CHECK(pw3_file_map_verify(9,PW3_FILE_SYSTEM,253,1,179,4,16000016,&reply_map));
 for(reply_fault=1;reply_fault<=27;reply_fault++)CHECK(!pw3_file_map_verify(9,PW3_FILE_SYSTEM,253,1,179,4,16000016,&reply_map));
 reply_fault=0;
 /* The report area, verified as itself. Its name, uuid and write access are
  * the other kind's, and a reply that is right for the system image is wrong
  * for this one: that is the whole reason the kind is a parameter. */
 reply_kind=PW3_FILE_REPORT;reply_map.sectors=PW3_TEST_REPORT_BYTES/512;reply_map.count=2;
 reply_map.extents[0]=(struct pw3_file_extent){0,100,64};
 reply_map.extents[1]=(struct pw3_file_extent){64,200,64};
 memset(&reply_map.extents[2],0,sizeof(reply_map.extents[2]));
 CHECK(pw3_file_map_verify(9,PW3_FILE_REPORT,253,1,179,4,16000016,&reply_map));
 CHECK(!pw3_file_map_verify(9,PW3_FILE_SYSTEM,253,1,179,4,16000016,&reply_map));
 CHECK(!pw3_file_map_verify(9,PW3_FILE_DATA,253,1,179,4,16000016,&reply_map));
 for(reply_fault=1;reply_fault<=27;reply_fault++)CHECK(!pw3_file_map_verify(9,PW3_FILE_REPORT,253,1,179,4,16000016,&reply_map));
 reply_fault=0;reply_kind=PW3_FILE_SYSTEM;
 /* No two of the three share a name, a uuid or a minor: dm-0 is the bounded
  * view of the volume, so the files are 1, 2 and 3. */
 for(unsigned a=0;a<PW3_FILE_KINDS;a++){
  CHECK(pw3_file_map_minor((enum pw3_file_kind)a)==a+1);
  for(unsigned b=0;b<a;b++){
   CHECK(strcmp(pw3_file_map_name((enum pw3_file_kind)a),pw3_file_map_name((enum pw3_file_kind)b)));
   CHECK(strcmp(pw3_file_map_uuid((enum pw3_file_kind)a),pw3_file_map_uuid((enum pw3_file_kind)b)));}}
 printf("MULTIEXTENT_GUARD_TEST_PASS checks=%u\n",checks);return 0;
}
