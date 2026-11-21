#pragma once

#include "ring_buffer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct WasapiEndpointInfo {
    std::string id;   // Windows MMDevice endpoint ID
    std::string name; // Friendly name (e.g. "CABLE Input (VB-Audio Virtual Cable)")
};

// Enumerates active Windows WASAPI Render (playback) endpoints.
// Used strictly for the optional Broadcast Send target (e.g., VB-Cable for Discord/OBS).
std::vector<WasapiEndpointInfo> enumerateWasapiRenderEndpoints();

struct WasapiSenderTelemetry {
    bool enabled = false;
    bool active = false;
    std::string deviceName;
    std::string error;
    AdaptiveAudioRingBuffer::Telemetry ring;
};

class WasapiSender {
public:
    WasapiSender() = default;
    ~WasapiSender();

    WasapiSender(const WasapiSender&) = delete;
    WasapiSender& operator=(const WasapiSender&) = delete;

    // Starts the background WASAPI render thread targeting `deviceId`.
    // Returns false if deviceId is empty; otherwise launches async worker.
    bool start(const std::string& deviceId, const std::string& deviceName, double sampleRate, size_t blockFrames);
    void stop();

    // Real-time safe (lock-free, zero allocations).. called from ASIO callback.
    void push(const float* ch0, const float* ch1, size_t numFrames);

    void resetXruns();
    WasapiSenderTelemetry getTelemetry() const;

private:
    void workerMain(std::string deviceId, double sampleRate);

    mutable std::mutex statusMutex_;
    std::string deviceName_;
    std::string lastError_;

    std::atomic<bool> enabled_{false};
    std::atomic<bool> active_{false};
    std::atomic<bool> stopRequested_{false};

    AdaptiveAudioRingBuffer ring_;
    std::thread worker_;
};
