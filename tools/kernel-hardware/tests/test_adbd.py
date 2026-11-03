from pathlib import Path
import subprocess, tempfile, re, os, shutil
import platform
ROOT=Path(__file__).resolve().parents[1]
# Whichever C++ compiler this host has: clang++ on the Mac, g++ in the build VM.
CXX=os.environ.get('CXX') or ('clang++' if shutil.which('clang++') else 'g++')
def functions(text,name):
    start=text.index(name);start=text.rfind('\n',0,start)+1
    pos=text.index('{',start);depth=1;end=pos+1
    while depth:
        if text[end]=='{': depth+=1
        elif text[end]=='}': depth-=1
        end+=1
    return text[start:end]+'\n'
def test(flags):
    original=(ROOT/'adb-contract/sources/adb__daemon__main.cpp').read_text()
    modified=(ROOT/'userspace-patch/patched-source/adb/daemon/main.cpp').read_text()
    core='\n'.join(functions(modified,n) for n in ('static void drop_privileges(', 'static void setup_port(', 'int adbd_main('))
    # setup_port's guard is outside its function, so retain it for the actual build.
    block=functions(modified,'static void setup_port(')
    core=core.replace(block,'#ifndef PW3_USB_ONLY_ADBD\n'+block+'#endif\n')
    prefix=r'''
#include <cassert>
#include <string>
#include <map>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <signal.h>
#include <unistd.h>
static std::map<std::string,std::string> props;
static bool auth_required=true, drop=true, usb_exists=true, open_ok=true;
static int tcp=0, smart=0, usb=0, opened=0, closed=0, access_count=0, events=0, authcalls=0, enters=0;
static const char* root_seclabel=nullptr;
#define ALLOW_ADBD_NO_AUTH 1
#define D(...) ((void)0)
struct Log { template<class T> Log& operator<<(T) { return *this; } };
#define LOG(x) Log()
#define PLOG(x) Log()
#define USB_FFS_ADB_EP0 "/dev/usb-ffs/adb/ep0"
#define DEFAULT_ADB_LOCAL_TRANSPORT_PORT 5555
#define AID_ADB 1
#define AID_LOG 2
#define AID_INPUT 3
#define AID_INET 4
#define AID_NET_BT 5
#define AID_NET_BT_ADMIN 6
#define AID_SDCARD_R 7
#define AID_SDCARD_RW 8
#define AID_NET_BW_STATS 9
#define AID_READPROC 10
#define AID_UHID 11
#define AID_SHELL 2000
#define arraysize(a) (sizeof(a)/sizeof((a)[0]))
struct ScopedMinijail {explicit ScopedMinijail(int*){} int* get(){return nullptr;}};
static int* minijail_new(){return nullptr;}
static void minijail_set_supplementary_gids(int*,size_t,gid_t*){}
static void drop_capabilities_bounding_set_if_needed(int*){}
static void minijail_change_gid(int*,int){}
static void minijail_change_uid(int*,int){}
static void minijail_enter(int*){++enters;}
static bool should_drop_privileges(){return drop;}
static int selinux_android_setcon(const char*){return 0;}
namespace android {namespace base {
static std::string GetProperty(const char* key,const char* def){return props.count(key)?props[key]:def;}
static bool GetBoolProperty(const char* key,bool def){return props.count(key)?props[key]=="1":def;}
static std::string StringPrintf(const char*,int){return "tcp:5037";}
}}
static int install_listener(const std::string&,const char*,void*,int,void*,std::string*){++smart;return 0;}
static void local_init(int){++tcp;}
static void setup_mdns(int){}
static void init_transport_registration(){}
static void adbd_cloexec_auth_socket(){}
static void adbd_auth_init(){++authcalls;}
static int fake_access(const char*,int){++access_count;return usb_exists?0:-1;}
#define access fake_access
static int adb_open(const char* p,int flags){assert(std::string(p)==USB_FFS_ADB_EP0);assert(flags==(O_RDWR|O_CLOEXEC));assert(enters==1);++opened;return open_ok?9:-1;}
static int adb_close(int f){assert(f==9);++closed;return 0;}
static void usb_init(){++usb;}
static void init_jdwp(){}
static void fdevent_loop(){++events;}
'''
    suffix=r'''
int main(){
 for(bool root:{false,true}) for(const char* port:{"","-1","0","5555","5555junk","abc","2147483647"})
 for(const char* persistent:{"","5556"}) for(bool have_usb:{false,true}) {
    props={{"ro.adb.secure","1"},{"service.adb.tcp.port",port},{"persist.adb.tcp.port",persistent}};
    drop=!root;usb_exists=have_usb;open_ok=have_usb;auth_required=true;
    tcp=smart=usb=opened=closed=access_count=events=authcalls=enters=0;
    int rc=adbd_main(5037);assert(authcalls==1);assert(enters==1);assert(auth_required);
#ifdef PW3_USB_ONLY_ADBD
    assert(tcp==0&&smart==0&&access_count==0&&opened==1);
    assert(rc==(have_usb?0:1)&&usb==(have_usb?1:0)&&closed==(have_usb?1:0)&&events==(have_usb?1:0));
#else
    assert(rc==0&&smart==(root?1:0)&&opened==0&&usb==(have_usb?1:0)&&access_count==1);
    const char* chosen=*port?port:persistent;int value=0;bool chosen_port=sscanf(chosen,"%d",&value)==1&&value>0;
    assert(tcp==(chosen_port||!have_usb?1:0));
#endif
 }
#ifdef PW3_USB_ONLY_ADBD
 for(const char* optin:{"","0","true","1"}) for(const char* secure:{"0","1"}) {
    props={{"ro.boot.pw3.adb",optin},{"ro.adb.secure",secure}};auth_required=true;drop=true;open_ok=true;
    tcp=smart=usb=opened=closed=access_count=events=authcalls=enters=0;
    // No boot opt-in since 0005: Android's own switch starts adbd; authentication stays required.
    bool allowed=std::string(secure)=="1";
    assert(adbd_main(5037)==(allowed?0:1));assert(tcp==0&&smart==0&&opened==(allowed?1:0));
    assert(auth_required==(std::string(secure)=="1"));assert(authcalls==1);
 }
#endif
 puts("Exact adbd functions passed");
}
'''
    with tempfile.TemporaryDirectory(prefix='pw3-adbd-test-') as d:
        p=Path(d);(p/'test.cpp').write_text(prefix+core+suffix)
        subprocess.run([CXX,*flags,'-std=c++11','-fsanitize=address,undefined','-g','-Wno-unused-function','-Wno-unused-parameter',str(p/'test.cpp'),'-o',str(p/'test')],check=True)
        subprocess.run([str(p/'test')],check=True)
    # A board-flag-off preprocessor must preserve the entire original source.
    def select(source):
        out=[];skip=[]
        for line in source.splitlines():
            if line in ('#ifdef PW3_USB_ONLY_ADBD','#ifndef PW3_USB_ONLY_ADBD'):
                skip.append(line.startswith('#ifdef'));continue
            if skip and line=='#else': skip[-1]=not skip[-1];continue
            if skip and line=='#endif':skip.pop();continue
            if not any(skip):out.append(line)
        return '\n'.join(out)
    assert re.sub(r'\s+',' ',select(modified)).strip()==re.sub(r'\s+',' ',original).strip()
if __name__=='__main__':
    # macOS needs to be told where libc++ is; every other host's compiler
    # already knows, so asking Xcode there would only fail.
    base=[]
    if platform.system()=='Darwin':
        sdk=subprocess.check_output(['xcrun','--show-sdk-path'],text=True).strip()
        base=['-isystem',sdk+'/usr/include/c++/v1']
    test(base);test(base+['-DPW3_USB_ONLY_ADBD'])
