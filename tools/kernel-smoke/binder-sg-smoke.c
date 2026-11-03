/* SPDX-License-Identifier: MIT
 * Normal Binder IPC regression only: all requests and objects are valid.
 * Restricted to the disposable diskless VExpress -pw3-core-smoke kernel.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <linux/types.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#include "binder.h"

#define MAP_BYTES (1024u * 1024u)
#define HELLO_CODE 1u
#define PAYLOAD_CODE 2u
#define MAGIC 0x53574733u
static const char file_content[] = "Binder SG file contents from the client after fork.\n";
static const char request_text[] = "PW3 valid Binder SG request";
static const char reply_text[] = "PW3 valid Binder SG reply";
static unsigned checks;
static pid_t child_pid;
static const char *role = "client";
static int service_anchor;

struct connection { int fd; unsigned char *map; };
struct payload { uint32_t magic; int32_t fd; char text[64]; };
struct objects { struct binder_buffer_object buffer; struct binder_fd_array_object fda; };

static void fail(const char *what, int line)
{
    fprintf(stderr, "binder-sg-smoke FAIL role=%s line=%d operation=%s errno=%d\n", role, line, what, errno);
    if (child_pid > 0) kill(child_pid, SIGKILL);
    _exit(1);
}
#define REQUIRE(condition, name) do { ++checks; if (!(condition)) fail(name, __LINE__); } while (0)
static void timed_out(int sig)
{
    static const char message[] = "binder-sg-smoke FAIL timeout\n";
    (void)sig; (void)write(STDERR_FILENO, message, sizeof(message)-1);
    if (child_pid > 0) kill(child_pid, SIGKILL);
    _exit(124);
}
static binder_uintptr_t address(const void *p) { return (binder_uintptr_t)(uintptr_t)p; }
static int within_mapping(const struct connection *c, binder_uintptr_t p, size_t n)
{
    uintptr_t low=(uintptr_t)c->map, value=(uintptr_t)p;
    return value>=low && n<=MAP_BYTES && value-low<=MAP_BYTES-n;
}
static void write_all(int fd, const void *buffer, size_t n)
{
    const unsigned char *p=buffer;
    while (n) { ssize_t r=write(fd,p,n); if(r<0 && errno==EINTR)continue;
        REQUIRE(r>0,"coordination write"); p+=r; n-=(size_t)r; }
}
static void read_all(int fd, void *buffer, size_t n)
{
    unsigned char *p=buffer;
    while (n) { ssize_t r=read(fd,p,n); if(r<0 && errno==EINTR)continue;
        REQUIRE(r>0,"coordination read"); p+=r; n-=(size_t)r; }
}
static void binder_write(struct connection *c, const void *p, size_t n)
{
    struct binder_write_read bwr;
    memset(&bwr,0,sizeof(bwr)); bwr.write_size=n; bwr.write_buffer=address(p);
    int rc; do { rc=ioctl(c->fd,BINDER_WRITE_READ,&bwr); } while(rc<0 && errno==EINTR);
    REQUIRE(rc==0 && bwr.write_consumed==n,"Binder command consumed");
}
static void command(struct connection *c, uint32_t cmd, const void *arg, size_t n)
{
    unsigned char bytes[128];
    REQUIRE(n+sizeof(cmd)<=sizeof(bytes),"command encoding size");
    memcpy(bytes,&cmd,sizeof(cmd)); if(n)memcpy(bytes+sizeof(cmd),arg,n);
    binder_write(c,bytes,sizeof(cmd)+n);
}
static struct connection connect_binder(void)
{
    struct connection c; struct binder_version v={0}; unsigned max_threads=0;
    c.fd=open("/dev/hwbinder",O_RDWR|O_CLOEXEC);
    REQUIRE(c.fd>=0,"open hwbinder");
    REQUIRE(ioctl(c.fd,BINDER_VERSION,&v)==0 && v.protocol_version==7,"32-bit Binder protocol");
    REQUIRE(ioctl(c.fd,BINDER_SET_MAX_THREADS,&max_threads)==0,"single test looper");
    c.map=mmap(NULL,MAP_BYTES,PROT_READ,MAP_PRIVATE|MAP_NORESERVE,c.fd,0);
    REQUIRE(c.map!=MAP_FAILED,"read-only Binder mapping");
    return c;
}
static void transact(struct connection *c, uint32_t cmd, uint32_t handle, uint32_t code,
                     void *data, size_t length, binder_size_t *offsets, size_t nr_offsets,
                     size_t extra_buffers)
{
    struct binder_transaction_data_sg sg;
    memset(&sg,0,sizeof(sg));
    sg.transaction_data.target.handle=handle; sg.transaction_data.code=code;
    sg.transaction_data.flags=TF_ACCEPT_FDS;
    sg.transaction_data.data_size=length;
    sg.transaction_data.offsets_size=nr_offsets*sizeof(*offsets);
    sg.transaction_data.data.ptr.buffer=address(data);
    sg.transaction_data.data.ptr.offsets=address(offsets);
    sg.buffers_size=extra_buffers;
    command(c,cmd,&sg,sizeof(sg));
}
static struct binder_transaction_data receive(struct connection *c, uint32_t expected)
{
    for (;;) {
        unsigned char bytes[1024]; struct binder_write_read bwr;
        memset(&bwr,0,sizeof(bwr)); bwr.read_size=sizeof(bytes); bwr.read_buffer=address(bytes);
        int rc; do {rc=ioctl(c->fd,BINDER_WRITE_READ,&bwr);}while(rc<0 && errno==EINTR);
        REQUIRE(rc==0,"Binder read ioctl");
        size_t cursor=0;
        while(cursor<bwr.read_consumed) {
            uint32_t cmd; REQUIRE(bwr.read_consumed-cursor>=sizeof(cmd),"complete return command");
            memcpy(&cmd,bytes+cursor,sizeof(cmd)); cursor+=sizeof(cmd);
            size_t arg_bytes=_IOC_SIZE(cmd);
            REQUIRE(arg_bytes<=bwr.read_consumed-cursor,"complete return argument");
            if(cmd==expected) {
                struct binder_transaction_data result;
                REQUIRE(arg_bytes==sizeof(result),"transaction return UAPI size");
                memcpy(&result,bytes+cursor,sizeof(result));
                REQUIRE(within_mapping(c,result.data.ptr.buffer,result.data_size),"received data in Binder mapping");
                REQUIRE(within_mapping(c,result.data.ptr.offsets,result.offsets_size),"received offsets in Binder mapping");
                return result;
            }
            if(cmd==BR_INCREFS || cmd==BR_ACQUIRE) {
                struct binder_ptr_cookie ref; memcpy(&ref,bytes+cursor,sizeof(ref));
                command(c,cmd==BR_INCREFS?BC_INCREFS_DONE:BC_ACQUIRE_DONE,&ref,sizeof(ref));
            } else {
                REQUIRE(cmd==BR_NOOP || cmd==BR_TRANSACTION_COMPLETE || cmd==BR_SPAWN_LOOPER ||
                        cmd==BR_RELEASE || cmd==BR_DECREFS,"normal Binder progress response");
            }
            cursor+=arg_bytes;
        }
    }
}
static void free_buffer(struct connection *c, const struct binder_transaction_data *t)
{
    binder_uintptr_t buffer=t->data.ptr.buffer;
    command(c,BC_FREE_BUFFER,&buffer,sizeof(buffer));
}
static void send_payload(struct connection *c, uint32_t command_code, uint32_t handle,
                         int fd, const char *text)
{
    struct payload p; struct objects obj; binder_size_t offsets[2];
    memset(&p,0,sizeof(p)); p.magic=MAGIC; p.fd=fd; snprintf(p.text,sizeof(p.text),"%s",text);
    memset(&obj,0,sizeof(obj));
    obj.buffer.hdr.type=BINDER_TYPE_PTR; obj.buffer.buffer=address(&p); obj.buffer.length=sizeof(p);
    obj.fda.hdr.type=BINDER_TYPE_FDA; obj.fda.num_fds=1; obj.fda.parent=0;
    obj.fda.parent_offset=offsetof(struct payload,fd);
    offsets[0]=offsetof(struct objects,buffer); offsets[1]=offsetof(struct objects,fda);
    transact(c,command_code,handle,PAYLOAD_CODE,&obj,sizeof(obj),offsets,2,(sizeof(p)+7u)&~7u);
}
static int check_payload(struct connection *c, const struct binder_transaction_data *t, const char *expected)
{
    struct objects obj; struct payload p; binder_size_t offsets[2]; char content[sizeof(file_content)];
    REQUIRE(t->code==PAYLOAD_CODE && t->data_size==sizeof(obj) && t->offsets_size==sizeof(offsets),"SG metadata dimensions");
    memcpy(&obj,(void *)(uintptr_t)t->data.ptr.buffer,sizeof(obj));
    memcpy(offsets,(void *)(uintptr_t)t->data.ptr.offsets,sizeof(offsets));
    REQUIRE(offsets[0]==offsetof(struct objects,buffer) && offsets[1]==offsetof(struct objects,fda),"PTR and FDA metadata offsets");
    REQUIRE(obj.buffer.hdr.type==BINDER_TYPE_PTR && obj.buffer.length==sizeof(p) &&
            obj.fda.hdr.type==BINDER_TYPE_FDA && obj.fda.num_fds==1 && obj.fda.parent==0 &&
            obj.fda.parent_offset==offsetof(struct payload,fd),"PTR and one-FD array received");
    REQUIRE(within_mapping(c,obj.buffer.buffer,sizeof(p)),"PTR translated into receiver mapping");
    memcpy(&p,(void *)(uintptr_t)obj.buffer.buffer,sizeof(p));
    REQUIRE(p.magic==MAGIC && !strcmp(p.text,expected),"received buffer contents");
    REQUIRE((fcntl(p.fd,F_GETFL)&O_ACCMODE)==O_RDONLY,"received file is readable read-only FD");
    REQUIRE((fcntl(p.fd,F_GETFD)&FD_CLOEXEC)!=0,"received FD close-on-exec");
    REQUIRE(lseek(p.fd,0,SEEK_SET)==0,"seek received regular file");
    memset(content,0,sizeof(content));
    REQUIRE(read(p.fd,content,sizeof(content))==(ssize_t)(sizeof(file_content)-1) &&
            !memcmp(content,file_content,sizeof(file_content)-1),"read actual transferred file contents");
    /* FDA descriptors belong to the received Binder buffer. BC_FREE_BUFFER
     * closes them; keep the buffer alive until use/reply has finished. */
    return p.fd;
}
static int make_fixture(void)
{
    const char *path="/tmp/binder-sg-normal-fixture";
    int writer=open(path,O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,0600);
    REQUIRE(writer>=0,"create RAM fixture after fork");
    write_all(writer,file_content,sizeof(file_content)-1); REQUIRE(close(writer)==0,"close fixture writer");
    int reader=open(path,O_RDONLY|O_CLOEXEC); REQUIRE(reader>=0,"open fixture read-only");
    REQUIRE(unlink(path)==0,"unlink RAM fixture pathname"); return reader;
}
static void server(int ready_fd, int done_fd)
{
    role="server"; checks=0; child_pid=0;
    struct connection c=connect_binder(); int rc=ioctl(c.fd,BINDER_SET_CONTEXT_MGR,0);
    REQUIRE(rc==0,"register test context manager"); command(&c,BC_ENTER_LOOPER,NULL,0);
    char ready='R'; write_all(ready_fd,&ready,1);
    struct binder_transaction_data hello=receive(&c,BR_TRANSACTION);
    REQUIRE(hello.code==HELLO_CODE && hello.data_size==sizeof(uint32_t),"service lookup request");
    uint32_t token; memcpy(&token,(void *)(uintptr_t)hello.data.ptr.buffer,sizeof(token));
    REQUIRE(token==MAGIC,"lookup request contents");
    struct flat_binder_object service; binder_size_t offset=0; memset(&service,0,sizeof(service));
    service.hdr.type=BINDER_TYPE_BINDER; service.flags=FLAT_BINDER_FLAG_ACCEPTS_FDS;
    service.binder=address(&service_anchor); service.cookie=MAGIC;
    transact(&c,BC_REPLY_SG,0,HELLO_CODE,&service,sizeof(service),&offset,1,0);
    free_buffer(&c,&hello);
    struct binder_transaction_data request=receive(&c,BR_TRANSACTION);
    REQUIRE(request.sender_pid==getppid(),"separate client process sender");
    int received_fd=check_payload(&c,&request,request_text);
    puts("binder-sg-smoke PASS server received PTR contents and read transferred FDA file");
    send_payload(&c,BC_REPLY_SG,0,received_fd,reply_text);
    free_buffer(&c,&request);
    char done; read_all(done_fd,&done,1); REQUIRE(done=='D',"client completed reply check");
    write_all(ready_fd,&checks,sizeof(checks));
    munmap(c.map,MAP_BYTES); close(c.fd); close(ready_fd); close(done_fd); _exit(0);
}
static int isolated_environment(void)
{
    struct utsname u; char cpu[8192];
    if(geteuid()!=0 || getppid()!=1 || uname(&u) || !strstr(u.release,"-pw3-core-smoke") || strncmp(u.machine,"arm",3))return 0;
    int fd=open("/proc/cpuinfo",O_RDONLY|O_CLOEXEC); if(fd<0)return 0;
    ssize_t n=read(fd,cpu,sizeof(cpu)-1); close(fd); if(n<=0)return 0;
    cpu[n]=0; return strstr(cpu,"ARM-Versatile Express")!=NULL;
}
int main(int argc,char **argv)
{
    setvbuf(stdout,NULL,_IONBF,0); setvbuf(stderr,NULL,_IONBF,0);
    if(argc!=2 || strcmp(argv[1],"--run-isolated") || !isolated_environment()) {
        fputs("binder-sg-smoke REFUSED: requires PID1 child in disposable ARM VExpress -pw3-core-smoke initramfs and --run-isolated\n",stderr); return 77;
    }
    signal(SIGALRM,timed_out); alarm(20);
    int ready[2],done[2]; REQUIRE(pipe(ready)==0 && pipe(done)==0,"coordination pipes");
    child_pid=fork(); REQUIRE(child_pid>=0,"two independent test processes");
    if(child_pid==0) { close(ready[0]); close(done[1]); alarm(20); server(ready[1],done[0]); }
    close(ready[1]); close(done[0]); char mark; read_all(ready[0],&mark,1); REQUIRE(mark=='R',"server ready");
    struct connection c=connect_binder(); uint32_t token=MAGIC;
    transact(&c,BC_TRANSACTION_SG,0,HELLO_CODE,&token,sizeof(token),NULL,0,0);
    struct binder_transaction_data hello=receive(&c,BR_REPLY); struct flat_binder_object service;
    REQUIRE(hello.data_size==sizeof(service) && hello.offsets_size==sizeof(binder_size_t),"service lookup reply dimensions");
    memcpy(&service,(void *)(uintptr_t)hello.data.ptr.buffer,sizeof(service));
    REQUIRE(service.hdr.type==BINDER_TYPE_HANDLE && service.handle>0,"service Binder handle received");
    uint32_t handle=service.handle; command(&c,BC_INCREFS,&handle,sizeof(handle)); command(&c,BC_ACQUIRE,&handle,sizeof(handle));
    free_buffer(&c,&hello);
    int original_fd=make_fixture();
    send_payload(&c,BC_TRANSACTION_SG,handle,original_fd,request_text);
    struct binder_transaction_data reply=receive(&c,BR_REPLY);
    (void)check_payload(&c,&reply,reply_text);
    puts("binder-sg-smoke PASS client received SG reply PTR contents and read returned FDA file");
    free_buffer(&c,&reply); close(original_fd);
    command(&c,BC_RELEASE,&handle,sizeof(handle)); command(&c,BC_DECREFS,&handle,sizeof(handle));
    mark='D'; write_all(done[1],&mark,1); unsigned server_checks; read_all(ready[0],&server_checks,sizeof(server_checks));
    int status; REQUIRE(waitpid(child_pid,&status,0)==child_pid && WIFEXITED(status) && WEXITSTATUS(status)==0,"server normal exit");
    child_pid=0; munmap(c.map,MAP_BYTES); close(c.fd); close(ready[0]); close(done[1]); alarm(0);
    printf("binder-sg-smoke SUMMARY: checks=%u failed=0; all requests valid; two processes; real Binder ioctl\n",checks+server_checks);
    puts("PW3_BINDER_SG_NORMAL_PASS"); return 0;
}
