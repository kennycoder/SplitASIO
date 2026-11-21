#pragma once

#include <string>
#include <vector>
#include <windows.h>

struct AsioDriverEntry {
    std::string name;         // Registry subkey name (e.g. "Focusrite USB ASIO")
    std::string description;  // Registry Description value
    std::string clsidStr;     // "{...}"
    CLSID clsid{};
};

// Enumerates installed 64-bit ASIO drivers under HKLM\SOFTWARE\ASIO.
// If excludeSelf is true, omits CLSID_AsioBridge to prevent recursive loading.
std::vector<AsioDriverEntry> enumerateAsioDrivers(bool excludeSelf = true);

// Finds a driver entry by exact or case-insensitive name match.
bool findAsioDriverByName(const std::string& name, AsioDriverEntry& outEntry, bool excludeSelf = true);
