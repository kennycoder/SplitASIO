#pragma once

#include "asio_abi.h"
#include "shared_tap.h"

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// Companion pure-ASIO input driver ("SplitASIO Tap") that reads stereo float32 audio
// from SplitASIO's cross-process shared-memory ring buffer.
// Allows OBS (via obs-asio) or a second DAW to capture processed SplitASIO audio
// directly over ASIO with zero virtual cables.
class TapDriver final : public IASIO {
public:
    TapDriver();
    ~TapDriver();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObject) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // IASIO
    ASIOBool init(void* sysHandle) override;
    void getDriverName(char* name) override;
    long getDriverVersion() override;
    void getErrorMessage(char* string) override;
    ASIOError start() override;
    ASIOError stop() override;
    ASIOError getChannels(long* numInputChannels, long* numOutputChannels) override;
    ASIOError getLatencies(long* inputLatency, long* outputLatency) override;
    ASIOError getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) override;
    ASIOError canSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getSampleRate(ASIOSampleRate* sampleRate) override;
    ASIOError setSampleRate(ASIOSampleRate sampleRate) override;
    ASIOError getClockSources(ASIOClockSource* clocks, long* numSources) override;
    ASIOError setClockSource(long reference) override;
    ASIOError getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) override;
    ASIOError getChannelInfo(ASIOChannelInfo* info) override;
    ASIOError createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) override;
    ASIOError disposeBuffers() override;
    ASIOError controlPanel() override;
    ASIOError future(long selector, void* opt) override;
    ASIOError outputReady() override;

private:
    void audioThreadMain();

    std::atomic<ULONG> refCount_{1};
    mutable std::mutex stateMutex_;

    bool initialized_ = false;
    bool buffersCreated_ = false;
    std::string lastError_;

    double sampleRate_ = 48000.0;
    long bufferSize_ = 128;

    ASIOCallbacks hostCallbacks_{};
    bool hostSupportsTimeInfo_ = false;

    bool channelActive_[2]{ false, false };
    std::vector<float> buf0_[2];
    std::vector<float> buf1_[2];

    SharedTapSubscriber subscriber_;
    std::atomic<bool> streaming_{false};
    std::atomic<uint64_t> samplePosition_{0};
    std::thread audioThread_;
};
