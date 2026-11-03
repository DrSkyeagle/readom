// SPDX-License-Identifier: Apache-2.0
#include "UsbGate.h"
#include "PosixChecks.h"
#include <android-base/properties.h>
#include <log/log.h>
#include <cerrno>
#include <time.h>

namespace {
class DevicePlatform : public pw3::Platform {
 public:
    std::string Get(const char* name) override { return android::base::GetProperty(name, ""); }
    bool Set(const char* name, const std::string& value) override {
        return android::base::SetProperty(name, value);
    }
    pw3::Presence Module(const char* name) override {
        return pw3::DirectoryPresence(std::string("/sys/module/") + name);
    }
    bool ModuleLive(const char* name) override {
        std::string state;
        return Module(name) == pw3::Presence::Present &&
               pw3::ReadSmallFile(std::string("/sys/module/") + name + "/initstate", 32, &state) &&
               state == "live\n";
    }
    bool DriverBound(const char* name) override {
        return pw3::ExactBinding(std::string("/sys/bus/platform/devices/") + name + "/driver",
                                 std::string("/sys/bus/platform/drivers/") + name);
    }
    bool FunctionFs() override {
        std::string text;
        return pw3::ReadSmallFile("/proc/filesystems", 16384, &text) && pw3::ContainsFunctionFs(text);
    }
    bool AndroidUsb() override {
        // The class entry is a link to /sys/devices/virtual/android_usb/android0.
        std::string enable;
        return pw3::ReadSmallFile("/sys/class/android_usb/android0/enable", 8, &enable) &&
               (enable == "0\n" || enable == "1\n");
    }
    bool RandomSerial(std::string* value) override { return pw3::ReadRandomSerial("/dev/urandom", value); }
    void Wait() override {
        struct timespec remaining = {0, 20000000};
        while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {}
    }
};
}
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    DevicePlatform platform;
    if (!pw3::RunUsbGate(platform, argv[1])) {
        ALOGE("PW3 USB module gate failed at stage %s; USB stays off", argv[1]);
        return 1;
    }
    // Never log the transient USB serial or any device identifiers.
    return 0;
}
