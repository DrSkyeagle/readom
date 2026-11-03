// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "UsbGate.h"
#include <cstddef>
#include <string>
namespace pw3 {
bool ReadSmallFile(const std::string&, size_t maximum, std::string*);
Presence DirectoryPresence(const std::string&);
bool ExactBinding(const std::string& link, const std::string& expected);
bool ContainsFunctionFs(const std::string& text);
bool ReadRandomSerial(const std::string& path, std::string* serial);
}
