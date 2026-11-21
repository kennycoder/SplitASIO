#pragma once

#include <string>

struct BridgeConfig {
    std::string inputDriver;
    std::string outputDriver;
    // 0 = Auto (recommended: 1.5x max(inBuf, outBuf), at least outBuf + 64)
    int ringBufferFrames = 0;
};

std::wstring getConfigFilePath();
BridgeConfig loadBridgeConfig();
bool saveBridgeConfig(const BridgeConfig& cfg);
