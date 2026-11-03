// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace pw3fg {

constexpr size_t kSnapshotSize = 32;
constexpr size_t kRecordSize = 64;
using Snapshot = std::array<uint8_t, kSnapshotSize>;
using Record = std::array<uint8_t, kRecordSize>;

// Reasons are stable labels. No learned register values are logged.
struct Result {
    bool ok;
    const char* reason;
    int error;
};
Result ParseSnapshot(const uint8_t* bytes, size_t size, Snapshot* out);
Result ReadSnapshot(const std::string& path, Snapshot* out);
Result MaySave(const Snapshot& snapshot);
// Writing the gauge's state back was built as far as the encoder and the
// atomic writer, never finished and never called; it went on 2026-10-29.
// MaySave stays because that is what --observe reports.

}  // namespace pw3fg
