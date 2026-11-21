#include "config.h"
#include "asio_registry.h"

#include <shlobj.h>
#include <windows.h>

namespace {

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (len <= 0) return {};
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), len);
    return out;
}

std::string wideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string out(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), len, nullptr, nullptr);
    return out;
}

std::wstring getConfigDir() {
    PWSTR appData = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        dir = std::wstring(appData) + L"\\SplitASIO";
        CoTaskMemFree(appData);
    } else {
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        dir = std::wstring(temp) + L"\\SplitASIO";
    }
    return dir;
}

} // namespace

std::wstring getConfigFilePath() {
    return getConfigDir() + L"\\config.ini";
}

BridgeConfig loadBridgeConfig() {
    BridgeConfig cfg;
    std::wstring path = getConfigFilePath();

    wchar_t buf[512]{};
    if (GetPrivateProfileStringW(L"Devices", L"InputDriver", L"", buf, 512, path.c_str()) > 0) {
        cfg.inputDriver = wideToUtf8(buf);
    }
    if (GetPrivateProfileStringW(L"Devices", L"OutputDriver", L"", buf, 512, path.c_str()) > 0) {
        cfg.outputDriver = wideToUtf8(buf);
    }
    cfg.ringBufferFrames = static_cast<int>(GetPrivateProfileIntW(L"Audio", L"RingBufferFrames", 0, path.c_str()));

    // First-run fallback: if no devices are configured yet, pick the first two installed ASIO drivers.
    if (cfg.inputDriver.empty() || cfg.outputDriver.empty()) {
        auto drivers = enumerateAsioDrivers(true);
        if (cfg.inputDriver.empty() && !drivers.empty()) {
            cfg.inputDriver = drivers[0].name;
        }
        if (cfg.outputDriver.empty() && !drivers.empty()) {
            for (const auto& d : drivers) {
                if (d.name != cfg.inputDriver) {
                    cfg.outputDriver = d.name;
                    break;
                }
            }
            if (cfg.outputDriver.empty()) {
                cfg.outputDriver = drivers[0].name;
            }
        }
    }

    return cfg;
}

bool saveBridgeConfig(const BridgeConfig& cfg) {
    std::wstring dir = getConfigDir();
    CreateDirectoryW(dir.c_str(), nullptr);

    std::wstring path = dir + L"\\config.ini";
    std::wstring inW = utf8ToWide(cfg.inputDriver);
    std::wstring outW = utf8ToWide(cfg.outputDriver);
    std::wstring rbW = std::to_wstring(cfg.ringBufferFrames);

    BOOL ok1 = WritePrivateProfileStringW(L"Devices", L"InputDriver", inW.c_str(), path.c_str());
    BOOL ok2 = WritePrivateProfileStringW(L"Devices", L"OutputDriver", outW.c_str(), path.c_str());
    BOOL ok3 = WritePrivateProfileStringW(L"Audio", L"RingBufferFrames", rbW.c_str(), path.c_str());
    return ok1 && ok2 && ok3;
}
