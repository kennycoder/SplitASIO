#pragma once

#include "config.h"
#include "ring_buffer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <windows.h>

struct BridgeStatusSnapshot {
    bool streaming = false;
    double sampleRate = 0.0;
    long inputBufferSize = 0;
    long outputBufferSize = 0;
    std::string inputDriverName;
    std::string outputDriverName;
    AdaptiveAudioRingBuffer::Telemetry ring;
};

struct ControlPanelHost {
    virtual ~ControlPanelHost() = default;
    virtual BridgeConfig getConfig() const = 0;
    virtual void applyConfig(const BridgeConfig& cfg) = 0;
    virtual void openInputDriverPanel() = 0;
    virtual void openOutputDriverPanel() = 0;
    virtual void resetXruns() = 0;
    virtual BridgeStatusSnapshot getStatus() const = 0;
};

class ControlPanel {
public:
    ControlPanel() = default;
    ~ControlPanel();

    ControlPanel(const ControlPanel&) = delete;
    ControlPanel& operator=(const ControlPanel&) = delete;

    void show(ControlPanelHost* host, HWND ownerHwnd);
    void runModal(ControlPanelHost* host);
    void close();

private:
    void uiThreadMain(ControlPanelHost* host);

    std::mutex mutex_;
    std::thread uiThread_;
    std::atomic<HWND> hwnd_{nullptr};
    std::atomic<bool> running_{false};
};
