// SPDX-License-Identifier: Apache-2.0
#include "UsbGate.h"

namespace pw3 {
namespace {
const char kPhase[] = "sys.pw3.usb.phase";
const char kSerial[] = "persist.pw3.usb.serial";
// g_ffs too: the Android gadget registers the functionfs file system itself.
const char* const kCompeting[] = {
    "g_ffs", "g_ether", "g_file_storage", "g_serial", "g_mass_storage", "g_multi",
    "g_zero", "g_hid", "g_audio", "g_webcam", "g_printer", "g_cdc",
    "g_ncm", "g_acm_ms", "g_nokia", "g_dbgp", "g_midi"
};
// USB is part of every boot (MTP; adb when Android's own switch turns it on), but
// only on a build whose adb requires authentication.
bool Allowed(Platform& p) {
    return p.Get("ro.secure") == "1" && p.Get("ro.adb.secure") == "1";
}
bool NoCompetitor(Platform& p) {
    for (const char* module : kCompeting)
        if (p.Module(module) != Presence::Absent) return false;
    return true;
}
bool Otg(Platform& p) {
    return p.ModuleLive("fsl_otg_arc") && p.DriverBound("fsl-usb2-otg");
}
bool Udc(Platform& p) {
    return Otg(p) && p.ModuleLive("arcotg_udc") && p.DriverBound("fsl-usb2-udc");
}
bool Gadget(Platform& p) {
    return Udc(p) && p.ModuleLive("g_android") && p.AndroidUsb() && p.FunctionFs();
}
bool Fail(Platform& p) { p.Set(kPhase, "error"); return false; }
}

bool ValidSerial(const std::string& serial) {
    if (serial.size() != 32) return false;
    bool nonzero = false;
    for (char c : serial) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
        nonzero |= c != '0';
    }
    return nonzero;
}

bool RunUsbGate(Platform& p, const std::string& stage) {
    if (!Allowed(p) || !NoCompetitor(p)) return Fail(p);
    if (stage == "prepare") {
        if (p.Get(kPhase) != "none") return Fail(p);
        for (const char* module : {"fsl_otg_arc", "arcotg_udc", "g_android"})
            if (p.Module(module) != Presence::Absent) return Fail(p);
        return p.Set(kPhase, "prepared");
    }
    if (stage == "serial") {
        // The USB serial: random, made once per device and kept (a computer then knows
        // the device again, instead of seeing a new one at every boot); never the
        // factory serial. Needs persistent properties, so it runs at boot.
        if (p.Get(kPhase) != "gadget-bound" || !Gadget(p)) return Fail(p);
        if (!ValidSerial(p.Get(kSerial))) {
            std::string serial;
            if (!p.RandomSerial(&serial) || !ValidSerial(serial) ||
                !p.Set(kSerial, serial) || p.Get(kSerial) != serial) return Fail(p);
        }
        return p.Set(kPhase, "ready");
    }
    const char* expected = nullptr;
    const char* next = nullptr;
    if (stage == "otg") { expected = "prepared"; next = "otg-bound"; }
    else if (stage == "udc") { expected = "otg-bound"; next = "udc-bound"; }
    else if (stage == "gadget") { expected = "udc-bound"; next = "gadget-bound"; }
    else return Fail(p);
    if (p.Get(kPhase) != expected) return Fail(p);
    // Module insertion may return success despite a failed platform probe.
    // Retry 100 times at 20 ms intervals; never unload/reset/power-toggle after failure.
    for (unsigned i = 0; i <= 100; ++i) {
        if (!Allowed(p) || !NoCompetitor(p) || p.Get(kPhase) != expected)
            return Fail(p);
        bool ready = stage == "otg" ? Otg(p) : stage == "udc" ? Udc(p) : Gadget(p);
        if (ready) return p.Set(kPhase, next);
        if (i != 100) p.Wait();
    }
    return Fail(p);
}
}
