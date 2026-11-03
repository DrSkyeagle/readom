// SPDX-License-Identifier: Apache-2.0
#include "FgState.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#if defined(__ANDROID__)
#include <android/log.h>
#endif

namespace {
constexpr const char* kSnapshot =
    "/sys/devices/system/wario_battery/wario_battery0/fg_android_snapshot";
void Report(const char* mode, const pw3fg::Result& result, int pending) {
    // Fixed reason labels and a POR flag only; never learned bytes or identifiers.
#if defined(__ANDROID__)
    __android_log_print(result.ok ? ANDROID_LOG_INFO : ANDROID_LOG_WARN, "pw3-fg-state",
                        "mode=%s result=%s error=%d restore_pending=%d",
                        mode, result.reason, result.error, pending);
#else
    std::printf("mode=%s result=%s error=%d restore_pending=%d\n",
                mode, result.reason, result.error, pending);
#endif
}
}
int main(int argc, char** argv) {
    std::string snapshot_path = kSnapshot;
    for (int i = 1; i < argc; ++i) {
        // --observe is what pw3-fg-state.rc starts it with, and is the only
        // thing this does: read the gauge and say whether its state could be
        // saved. Writing it back was built as far as the encoder and never
        // finished or used; it went on 2026-10-29.
        if (std::strcmp(argv[i], "--observe") == 0) continue;
        if (std::strcmp(argv[i], "--snapshot") == 0 && i + 1 < argc) {
            snapshot_path = argv[++i];
        } else {
            Report("read_only", {false, "argument_invalid", 0}, -1);
            return 2;
        }
    }
    pw3fg::Snapshot snapshot {};
    const pw3fg::Result read = pw3fg::ReadSnapshot(snapshot_path, &snapshot);
    if (!read.ok) {
        Report("read_only", read, -1);
        return 1;
    }
    const pw3fg::Result eligible = pw3fg::MaySave(snapshot);
    Report("read_only", eligible, snapshot[5]);
    return 0;  // Successfully observed; pending POR is diagnostic, not restored.
}
