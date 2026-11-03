#include "UsbGate.h"
#include "PosixChecks.h"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
using pw3::Presence;
struct Fake : pw3::Platform {
    std::map<std::string,std::string> props{{"ro.secure","1"},{"ro.adb.secure","1"},{"sys.pw3.usb.phase","none"}};
    std::map<std::string,Presence> modules;
    std::set<std::string> live,drivers;
    std::string failset, random="0123456789abcdef0123456789abcdef";
    bool randomok=true, fs=false, android=false;
    int waits=0, delayed=0, randoms=0;
    std::string Get(const char* n) override { return props[n]; }
    bool Set(const char* n,const std::string& v) override { if (n==failset) return false;props[n]=v;return true; }
    Presence Module(const char* n) override { return modules.count(n)?modules[n]:Presence::Absent; }
    bool ModuleLive(const char* n) override { return live.count(n); }
    bool DriverBound(const char* n) override { return drivers.count(n); }
    bool FunctionFs() override { return fs; }
    bool AndroidUsb() override { return android; }
    bool RandomSerial(std::string* v) override { ++randoms;*v=random;return randomok; }
    void Wait() override { ++waits;if (delayed && waits==delayed) {live.insert("fsl_otg_arc");drivers.insert("fsl-usb2-otg");} }
    void otg() {live.insert("fsl_otg_arc");drivers.insert("fsl-usb2-otg");}
    void udc() {otg();live.insert("arcotg_udc");drivers.insert("fsl-usb2-udc");}
    void gadget() {udc();live.insert("g_android");android=true;fs=true;}
    void bound() {assert(pw3::RunUsbGate(*this,"prepare"));otg();assert(pw3::RunUsbGate(*this,"otg"));udc();assert(pw3::RunUsbGate(*this,"udc"));gadget();assert(pw3::RunUsbGate(*this,"gadget"));assert(props["sys.pw3.usb.phase"]=="gadget-bound");}
};
void failed(Fake& f,const char* stage) { assert(!pw3::RunUsbGate(f,stage));assert(f.props["sys.pw3.usb.phase"]=="error"); }
int main() {
    // Every boot, no boot flag needed; only on a build whose adb requires authentication.
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));assert(f.props["sys.pw3.usb.phase"]=="prepared");}
    for (const char* prop:{"ro.secure","ro.adb.secure"})
        for (const char* value:{"","0","true","2"}) {Fake f;f.props[prop]=value;failed(f,"prepare");}
    for (const char* m:{"fsl_otg_arc","arcotg_udc","g_android","g_ffs","g_ether","g_file_storage","g_serial"})
        for (Presence p:{Presence::Present,Presence::Error}) {Fake f;f.modules[m]=p;failed(f,"prepare");}
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));failed(f,"prepare");}
    // The chain, with a delayed OTG binding.
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.delayed=7;assert(pw3::RunUsbGate(f,"otg"));assert(f.waits==7);f.udc();assert(pw3::RunUsbGate(f,"udc"));f.gadget();assert(pw3::RunUsbGate(f,"gadget"));assert(f.props["sys.pw3.usb.phase"]=="gadget-bound");}
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.live.insert("fsl_otg_arc");failed(f,"otg");assert(f.waits==100);}
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.drivers.insert("fsl-usb2-otg");failed(f,"otg");assert(f.waits==100);}
    for (const char* stage:{"udc","gadget","serial","ffs","bogus"}) {Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.gadget();failed(f,stage);}
    // The gadget stage needs the module, its android_usb class and functionfs.
    for (int missing=0;missing<3;++missing) {
        Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.otg();assert(pw3::RunUsbGate(f,"otg"));f.udc();assert(pw3::RunUsbGate(f,"udc"));
        f.gadget();if (missing==0) f.live.erase("g_android");if (missing==1) f.android=false;if (missing==2) f.fs=false;
        failed(f,"gadget");assert(f.waits==100);
    }
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.otg();f.modules["g_serial"]=Presence::Present;failed(f,"otg");assert(f.waits==0);}
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.otg();f.props["ro.adb.secure"]="0";failed(f,"otg");}
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));f.otg();f.failset="sys.pw3.usb.phase";assert(!pw3::RunUsbGate(f,"otg"));assert(f.props["sys.pw3.usb.phase"]=="prepared");}
    // The serial: made once, kept, never anything but 32 lowercase hex digits.
    {Fake f;f.bound();assert(pw3::RunUsbGate(f,"serial"));assert(f.props["persist.pw3.usb.serial"]==f.random);assert(f.randoms==1);assert(f.props["sys.pw3.usb.phase"]=="ready");}
    {Fake f;f.props["persist.pw3.usb.serial"]="fedcba9876543210fedcba9876543210";f.bound();assert(pw3::RunUsbGate(f,"serial"));assert(f.randoms==0);assert(f.props["persist.pw3.usb.serial"]=="fedcba9876543210fedcba9876543210");}
    for (const char* kept:{"", "0", "00000000000000000000000000000000", "0123456789abcdef0123456789abcdefx", "0123456789abcdef0123456789abcdeF"}) {
        Fake f;f.props["persist.pw3.usb.serial"]=kept;f.bound();assert(pw3::RunUsbGate(f,"serial"));assert(f.randoms==1);assert(f.props["persist.pw3.usb.serial"]==f.random);
    }
    for (const char* token:{"", "0", "00000000000000000000000000000000", "0123456789abcdef0123456789abcdefx", "0123456789abcdef0123456789abcdeF", "../../../../../../../../../../xx"}) {
        Fake f;f.random=token;f.bound();failed(f,"serial");
    }
    {Fake f;f.randomok=false;f.bound();failed(f,"serial");}
    {Fake f;f.failset="persist.pw3.usb.serial";f.bound();failed(f,"serial");}
    {Fake f;f.bound();f.android=false;failed(f,"serial");}
    {Fake f;assert(pw3::RunUsbGate(f,"prepare"));failed(f,"serial");assert(f.randoms==0);}
    char root[]="/tmp/pw3-usb-test-XXXXXX";assert(mkdtemp(root));std::string dir=root;
    const std::string file=dir+"/state", target=dir+"/target", wrong=dir+"/wrong", link=dir+"/driver";
    mkdir(target.c_str(),0700);mkdir(wrong.c_str(),0700);
    assert(pw3::DirectoryPresence(target)==Presence::Present);
    assert(pw3::DirectoryPresence(dir+"/absent")==Presence::Absent);
    {std::ofstream f(file);f<<"live\n";}
    assert(pw3::DirectoryPresence(file)==Presence::Error);
    std::string text="unchanged";assert(pw3::ReadSmallFile(file,32,&text)&&text=="live\n");
    text="unchanged";assert(!pw3::ReadSmallFile(file,2,&text)&&text=="unchanged");
    assert(symlink(target.c_str(),link.c_str())==0);
    assert(pw3::ExactBinding(link,target));assert(!pw3::ExactBinding(link,wrong));assert(!pw3::ExactBinding(target,target));
    assert(pw3::DirectoryPresence(link)==Presence::Error);assert(!pw3::ReadSmallFile(link,32,&text));
    assert(pw3::ContainsFunctionFs("nodev\tfunctionfs\n"));
    for (const char* s:{"functionfs\n","nodev functionfs_extra\n","nodev functionfs extra\n","nodev configfs\n"}) assert(!pw3::ContainsFunctionFs(s));
    std::string serial="preserve";assert(!pw3::ReadRandomSerial(file,&serial)&&serial=="preserve");
    {std::ofstream f(file,std::ios::binary);for(int i=0;i<16;++i)f.put(static_cast<char>(i));}
    assert(pw3::ReadRandomSerial(file,&serial)&&serial=="000102030405060708090a0b0c0d0e0f");
    unlink(link.c_str());unlink(file.c_str());rmdir(target.c_str());rmdir(wrong.c_str());rmdir(root);
    puts("USB gate and POSIX checks passed");
}
