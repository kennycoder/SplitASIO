#include "asio_registry.h"
#include "driver_guid.h"

#include <algorithm>
#include <cctype>
#include <objbase.h>

namespace {

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

bool readRegString(HKEY key, const wchar_t* valueName, std::wstring& out) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, valueName, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) {
        return false;
    }
    if (type != REG_SZ && type != REG_EXPAND_SZ) {
        return false;
    }
    std::wstring buf(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, valueName, nullptr, &type, reinterpret_cast<LPBYTE>(buf.data()), &size) != ERROR_SUCCESS) {
        return false;
    }
    while (!buf.empty() && buf.back() == L'\0') {
        buf.pop_back();
    }
    out = std::move(buf);
    return true;
}

bool equalsCaseInsensitive(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

} // namespace

std::vector<AsioDriverEntry> enumerateAsioDrivers(bool excludeSelf) {
    std::vector<AsioDriverEntry> result;

    HKEY rootKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\ASIO", 0, KEY_READ | KEY_WOW64_64KEY, &rootKey) != ERROR_SUCCESS) {
        return result;
    }

    DWORD index = 0;
    wchar_t subkeyName[256];
    for (;;) {
        DWORD nameLen = static_cast<DWORD>(std::size(subkeyName));
        LONG status = RegEnumKeyExW(rootKey, index++, subkeyName, &nameLen, nullptr, nullptr, nullptr, nullptr);
        if (status == ERROR_NO_MORE_ITEMS) {
            break;
        }
        if (status != ERROR_SUCCESS) {
            continue;
        }

        HKEY subKey = nullptr;
        if (RegOpenKeyExW(rootKey, subkeyName, 0, KEY_READ | KEY_WOW64_64KEY, &subKey) != ERROR_SUCCESS) {
            continue;
        }

        std::wstring clsidW;
        std::wstring descW;
        bool hasClsid = readRegString(subKey, L"CLSID", clsidW);
        readRegString(subKey, L"Description", descW);
        RegCloseKey(subKey);

        if (!hasClsid) {
            continue;
        }

        CLSID clsid{};
        if (FAILED(CLSIDFromString(clsidW.c_str(), &clsid))) {
            continue;
        }

        if (excludeSelf && (IsEqualCLSID(clsid, CLSID_AsioBridge) || IsEqualCLSID(clsid, CLSID_SplitAsioTap))) {
            continue;
        }

        AsioDriverEntry entry;
        entry.name = wideToUtf8(std::wstring(subkeyName, nameLen));
        entry.description = descW.empty() ? entry.name : wideToUtf8(descW);
        entry.clsidStr = wideToUtf8(clsidW);
        entry.clsid = clsid;
        result.push_back(std::move(entry));
    }

    RegCloseKey(rootKey);

    std::sort(result.begin(), result.end(), [](const AsioDriverEntry& a, const AsioDriverEntry& b) {
        return a.name < b.name;
    });
    return result;
}

bool findAsioDriverByName(const std::string& name, AsioDriverEntry& outEntry, bool excludeSelf) {
    auto drivers = enumerateAsioDrivers(excludeSelf);
    for (const auto& d : drivers) {
        if (d.name == name) {
            outEntry = d;
            return true;
        }
    }
    for (const auto& d : drivers) {
        if (equalsCaseInsensitive(d.name, name) || equalsCaseInsensitive(d.description, name)) {
            outEntry = d;
            return true;
        }
    }
    return false;
}
