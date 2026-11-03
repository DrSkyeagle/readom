// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>

namespace pw3 {
enum class Presence { Absent, Present, Error };
class Platform {
 public:
    virtual ~Platform() {}
    virtual std::string Get(const char*) = 0;
    virtual bool Set(const char*, const std::string&) = 0;
    virtual Presence Module(const char*) = 0;
    virtual bool ModuleLive(const char*) = 0;
    virtual bool DriverBound(const char*) = 0;
    virtual bool FunctionFs() = 0;
    virtual bool AndroidUsb() = 0;
    virtual bool RandomSerial(std::string*) = 0;
    virtual void Wait() = 0;
};
bool ValidSerial(const std::string& serial);
bool RunUsbGate(Platform& platform, const std::string& stage);
}
