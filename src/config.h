#pragma once

#include <string>

enum class TapSource : int {
    Disabled     = 0,
    MirrorMaster = 1, // Mirror hardware Output 1/2 (great for standalone amp sims)
    VirtualSend  = 2, // Expose extra Out 3/4 ("Broadcast Send L/R") for custom DAW submixes
};

struct BridgeConfig {
    std::string inputDriver;
    std::string outputDriver;
    // 0 = Auto (recommended: inBuf + outBuf + jitter margin)
    int ringBufferFrames = 0;

    // Broadcast / Stream output tap settings (all off by default for pure-ASIO operation)
    TapSource tapSource = TapSource::Disabled;
    bool enableAsioTap = false;     // Path B: Publish to "SplitASIO Tap" (pure-ASIO for OBS obs-asio)
    bool enableWasapiSend = false;  // Path A: Mirror via WASAPI to a virtual cable
    std::string wasapiDeviceId;
    std::string wasapiDeviceName;
};

std::wstring getConfigFilePath();
BridgeConfig loadBridgeConfig();
bool saveBridgeConfig(const BridgeConfig& cfg);
