#pragma once

#include "asio_abi.h"
#include "asio_host.h"
#include "config.h"
#include "control_panel.h"
#include "ring_buffer.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class BridgeDriver final : public IASIO, public ControlPanelHost {
public:
    BridgeDriver();
    ~BridgeDriver() override;

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

    // ControlPanelHost
    BridgeConfig getConfig() const override;
    void applyConfig(const BridgeConfig& cfg) override;
    void openInputDriverPanel() override;
    void openOutputDriverPanel() override;
    void resetXruns() override;
    BridgeStatusSnapshot getStatus() const override;

private:
    // Static C trampolines for child ASIO drivers
    static void cbInBufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
    static void cbInSampleRateDidChange(ASIOSampleRate sRate);
    static long cbInAsioMessage(long selector, long value, void* message, double* opt);
    static ASIOTime* cbInBufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);

    static void cbOutBufferSwitch(long doubleBufferIndex, ASIOBool directProcess);
    static void cbOutSampleRateDidChange(ASIOSampleRate sRate);
    static long cbOutAsioMessage(long selector, long value, void* message, double* opt);
    static ASIOTime* cbOutBufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);

    void onInputBufferSwitch(long doubleBufferIndex);
    ASIOTime* onOutputBufferSwitch(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess);
    long onChildAsioMessage(bool isInput, long selector, long value, void* message, double* opt);

    void setError(const std::string& msg);

    static std::atomic<BridgeDriver*> s_activeInstance_;

    std::atomic<ULONG> refCount_{1};
    mutable std::mutex stateMutex_;

    BridgeConfig config_;
    void* sysHandle_ = nullptr;
    bool initialized_ = false;
    std::string lastError_;

    AsioChildDriver inDriver_;
    AsioChildDriver outDriver_;

    double currentSampleRate_ = 48000.0;
    long inBufSize_ = 0;
    long outBufSize_ = 0;
    std::atomic<bool> streaming_{false};
    std::atomic<bool> outputReadyCalledByHost_{false};
    std::atomic<uint64_t> totalOutputFrames_{0};

    ASIOCallbacks hostCallbacks_{};
    bool hostSupportsTimeInfo_ = false;
    bool buffersCreated_ = false;

    // Per-channel metadata & host-visible input double buffers
    std::vector<ASIOSampleType> inSampleTypes_;
    std::vector<bool> inChannelActive_;
    std::vector<bool> outChannelActive_;
    std::vector<std::vector<uint8_t>> hostInBuf0_;
    std::vector<std::vector<uint8_t>> hostInBuf1_;

    // Pre-allocated real-time scratch buffers (zero allocations in callbacks)
    std::vector<std::vector<float>> pushScratch_;
    std::vector<const float*> pushScratchPtrs_;
    std::vector<std::vector<float>> pullScratch_;
    std::vector<float*> pullScratchPtrs_;

    AdaptiveAudioRingBuffer ringBuffer_;
    ControlPanel controlPanel_;
};
