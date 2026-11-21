#include "bridge_driver.h"
#include "driver_guid.h"
#include "sample_convert.h"

#include <algorithm>
#include <cstring>

extern void dllObjectCreated();
extern void dllObjectDestroyed();

std::atomic<BridgeDriver*> BridgeDriver::s_activeInstance_{nullptr};

BridgeDriver::BridgeDriver() {
    dllObjectCreated();
}

BridgeDriver::~BridgeDriver() {
    controlPanel_.close();
    stop();
    disposeBuffers();

    BridgeDriver* expected = this;
    s_activeInstance_.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);

    inDriver_.close();
    outDriver_.close();
    dllObjectDestroyed();
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE BridgeDriver::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    // ASIO's favorite COM party trick: instead of a shared interface ID, hosts ask for our own
    // CLSID as the IID. Play along or every DAW will pretend we don't exist.
    // Docs: https://learn.microsoft.com/en-us/windows/win32/api/unknwn/nf-unknwn-iunknown-queryinterface(refiid_void)
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_AsioBridge)) {
        *ppvObject = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE BridgeDriver::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE BridgeDriver::Release() {
    ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// IASIO
// ============================================================================

ASIOBool BridgeDriver::init(void* sysHandle) {
    std::lock_guard<std::mutex> lock(stateMutex_);

    sysHandle_ = sysHandle ? sysHandle : GetDesktopWindow();
    config_ = loadBridgeConfig();
    lastError_.clear();

    std::string err;
    if (!inDriver_.open(config_.inputDriver, sysHandle_, err)) {
        setError("Input device error: " + err);
        return ASIOFalse;
    }

    if (!outDriver_.open(config_.outputDriver, sysHandle_, err)) {
        inDriver_.close();
        setError("Output device error: " + err);
        return ASIOFalse;
    }

    if (inDriver_.numInputChannels() <= 0) {
        inDriver_.close();
        outDriver_.close();
        setError("Selected input ASIO driver has 0 input channels.");
        return ASIOFalse;
    }

    if (outDriver_.numOutputChannels() <= 0) {
        inDriver_.close();
        outDriver_.close();
        setError("Selected output ASIO driver has 0 output channels.");
        return ASIOFalse;
    }

    // Synchronize sample rate between output (master) and input
    ASIOSampleRate outSr = 48000.0;
    if (outDriver_.raw()->getSampleRate(&outSr) != ASE_OK || outSr <= 0.0) {
        outSr = 48000.0;
    }

    if (inDriver_.raw()->setSampleRate(outSr) != ASE_OK) {
        // Fallback: try 48000 then 44100 on both
        const double fallbacks[] = { 48000.0, 44100.0 };
        bool synced = false;
        for (double sr : fallbacks) {
            if (outDriver_.raw()->canSampleRate(sr) == ASE_OK &&
                inDriver_.raw()->canSampleRate(sr) == ASE_OK) {
                if (outDriver_.raw()->setSampleRate(sr) == ASE_OK &&
                    inDriver_.raw()->setSampleRate(sr) == ASE_OK) {
                    outSr = sr;
                    synced = true;
                    break;
                }
            }
        }
        if (!synced) {
            inDriver_.close();
            outDriver_.close();
            setError("Failed to synchronize sample rate between input and output ASIO devices.");
            return ASIOFalse;
        }
    }

    currentSampleRate_ = outSr;
    s_activeInstance_.store(this, std::memory_order_release);
    initialized_ = true;
    return ASIOTrue;
}

void BridgeDriver::getDriverName(char* name) {
    if (!name) return;
    strncpy_s(name, 32, kAsioBridgeNameA, _TRUNCATE);
}

long BridgeDriver::getDriverVersion() {
    return 100; // v1.0.0
}

void BridgeDriver::getErrorMessage(char* string) {
    if (!string) return;
    std::lock_guard<std::mutex> lock(stateMutex_);
    strncpy_s(string, 124, lastError_.c_str(), _TRUNCATE);
}

ASIOError BridgeDriver::getChannels(long* numInputChannels, long* numOutputChannels) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (numInputChannels)  *numInputChannels  = inDriver_.numInputChannels();
    if (numOutputChannels) *numOutputChannels = outDriver_.numOutputChannels();
    return ASE_OK;
}

ASIOError BridgeDriver::getLatencies(long* inputLatency, long* outputLatency) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;

    long inLatIn = 0, inLatOut = 0;
    long outLatIn = 0, outLatOut = 0;
    inDriver_.raw()->getLatencies(&inLatIn, &inLatOut);
    outDriver_.raw()->getLatencies(&outLatIn, &outLatOut);

    long bridgeMargin = static_cast<long>(ringBuffer_.targetFillFrames());
    if (inputLatency)  *inputLatency  = inLatIn + bridgeMargin;
    if (outputLatency) *outputLatency = outLatOut;
    return ASE_OK;
}

ASIOError BridgeDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    // Output device (FiiO) is master clock; expose its exact buffer constraints.
    return outDriver_.raw()->getBufferSize(minSize, maxSize, preferredSize, granularity);
}

ASIOError BridgeDriver::canSampleRate(ASIOSampleRate sampleRate) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (outDriver_.raw()->canSampleRate(sampleRate) != ASE_OK) return ASE_NoClock;
    if (inDriver_.raw()->canSampleRate(sampleRate) != ASE_OK) return ASE_NoClock;
    return ASE_OK;
}

ASIOError BridgeDriver::getSampleRate(ASIOSampleRate* sampleRate) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!sampleRate) return ASE_InvalidParameter;
    ASIOSampleRate sr = currentSampleRate_;
    if (outDriver_.raw()->getSampleRate(&sr) == ASE_OK && sr > 0.0) {
        currentSampleRate_ = sr;
    }
    *sampleRate = currentSampleRate_;
    return ASE_OK;
}

ASIOError BridgeDriver::setSampleRate(ASIOSampleRate sampleRate) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (sampleRate <= 0.0) return ASE_InvalidParameter;

    ASIOError eOut = outDriver_.raw()->setSampleRate(sampleRate);
    if (eOut != ASE_OK) {
        setError("Output ASIO driver rejected requested sample rate.");
        return eOut;
    }
    ASIOError eIn = inDriver_.raw()->setSampleRate(sampleRate);
    if (eIn != ASE_OK) {
        setError("Input ASIO driver rejected requested sample rate.");
        return eIn;
    }
    currentSampleRate_ = sampleRate;
    return ASE_OK;
}

ASIOError BridgeDriver::getClockSources(ASIOClockSource* clocks, long* numSources) {
    if (!clocks || !numSources || *numSources <= 0) return ASE_InvalidParameter;
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strncpy_s(clocks[0].name, sizeof(clocks[0].name), "Output ASIO Master Clock", _TRUNCATE);
    *numSources = 1;
    return ASE_OK;
}

ASIOError BridgeDriver::setClockSource(long reference) {
    return (reference == 0) ? ASE_OK : ASE_InvalidParameter;
}

ASIOError BridgeDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    return outDriver_.raw()->getSamplePosition(sPos, tStamp);
}

ASIOError BridgeDriver::getChannelInfo(ASIOChannelInfo* info) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!info) return ASE_InvalidParameter;

    if (info->isInput) {
        ASIOError err = inDriver_.raw()->getChannelInfo(info);
        if (err == ASE_OK) {
            bool active = (info->channel >= 0 &&
                           static_cast<size_t>(info->channel) < inChannelActive_.size() &&
                           inChannelActive_[static_cast<size_t>(info->channel)]);
            info->isActive = active ? ASIOTrue : ASIOFalse;
        }
        return err;
    } else {
        ASIOError err = outDriver_.raw()->getChannelInfo(info);
        if (err == ASE_OK) {
            bool active = (info->channel >= 0 &&
                           static_cast<size_t>(info->channel) < outChannelActive_.size() &&
                           outChannelActive_[static_cast<size_t>(info->channel)]);
            info->isActive = active ? ASIOTrue : ASIOFalse;
        }
        return err;
    }
}

ASIOError BridgeDriver::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!bufferInfos || numChannels <= 0 || bufferSize <= 0 || !callbacks) {
        return ASE_InvalidParameter;
    }

    if (buffersCreated_) {
        // Clean up previous buffers before recreating
        streaming_.store(false, std::memory_order_release);
        outDriver_.stop();
        inDriver_.stop();
        outDriver_.disposeBuffers();
        inDriver_.disposeBuffers();
        buffersCreated_ = false;
    }

    hostCallbacks_ = *callbacks;
    hostSupportsTimeInfo_ = false;
    if (hostCallbacks_.asioMessage && hostCallbacks_.bufferSwitchTimeInfo) {
        if (hostCallbacks_.asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1) {
            hostSupportsTimeInfo_ = (hostCallbacks_.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1);
        }
    }

    outDriver_.refreshCaps();
    inDriver_.refreshCaps();

    const long outChTotal = outDriver_.numOutputChannels();
    const long inChTotal  = inDriver_.numInputChannels();

    outBufSize_ = outDriver_.negotiateBufferSize(bufferSize);
    inBufSize_  = inDriver_.negotiateBufferSize(bufferSize);

    // Ensure s_activeInstance_ points to us before child drivers register callbacks
    s_activeInstance_.store(this, std::memory_order_release);

    static ASIOCallbacks inCbs{
        &BridgeDriver::cbInBufferSwitch,
        &BridgeDriver::cbInSampleRateDidChange,
        &BridgeDriver::cbInAsioMessage,
        &BridgeDriver::cbInBufferSwitchTimeInfo
    };
    static ASIOCallbacks outCbs{
        &BridgeDriver::cbOutBufferSwitch,
        &BridgeDriver::cbOutSampleRateDidChange,
        &BridgeDriver::cbOutAsioMessage,
        &BridgeDriver::cbOutBufferSwitchTimeInfo
    };

    // Create child output buffers (master clock)
    ASIOError err = outDriver_.createOutputBuffers(outChTotal, outBufSize_, &outCbs);
    if (err != ASE_OK) {
        setError("Failed to create buffers on output ASIO driver.");
        return err;
    }

    // Create child input buffers
    err = inDriver_.createInputBuffers(inChTotal, inBufSize_, &inCbs);
    if (err != ASE_OK) {
        outDriver_.disposeBuffers();
        setError("Failed to create buffers on input ASIO driver.");
        return err;
    }

    // Record input channel sample formats
    inSampleTypes_.assign(static_cast<size_t>(inChTotal), ASIOSTInt32LSB);
    for (long c = 0; c < inChTotal; ++c) {
        if (static_cast<size_t>(c) < inDriver_.inputChannelInfos().size()) {
            inSampleTypes_[static_cast<size_t>(c)] = inDriver_.inputChannelInfos()[static_cast<size_t>(c)].type;
        }
    }

    inChannelActive_.assign(static_cast<size_t>(inChTotal), false);
    outChannelActive_.assign(static_cast<size_t>(outChTotal), false);

    // Allocate host-visible input double buffers (zero-initialized)
    hostInBuf0_.resize(static_cast<size_t>(inChTotal));
    hostInBuf1_.resize(static_cast<size_t>(inChTotal));
    for (long c = 0; c < inChTotal; ++c) {
        size_t bytes = static_cast<size_t>(outBufSize_) * sample_convert::sampleByteSize(inSampleTypes_[static_cast<size_t>(c)]);
        hostInBuf0_[static_cast<size_t>(c)].assign(bytes, 0);
        hostInBuf1_[static_cast<size_t>(c)].assign(bytes, 0);
    }

    // Zero all child output buffers initially
    const auto& outChildBufs = outDriver_.bufferInfos();
    for (size_t c = 0; c < outChildBufs.size(); ++c) {
        ASIOSampleType st = (c < outDriver_.outputChannelInfos().size())
            ? outDriver_.outputChannelInfos()[c].type
            : ASIOSTInt32LSB;
        size_t bytes = static_cast<size_t>(outBufSize_) * sample_convert::sampleByteSize(st);
        if (outChildBufs[c].buffers[0]) std::memset(outChildBufs[c].buffers[0], 0, bytes);
        if (outChildBufs[c].buffers[1]) std::memset(outChildBufs[c].buffers[1], 0, bytes);
    }

    // Map DAW buffer pointers
    for (long i = 0; i < numChannels; ++i) {
        long ch = bufferInfos[i].channelNum;
        if (bufferInfos[i].isInput) {
            if (ch < 0 || ch >= inChTotal) {
                inDriver_.disposeBuffers();
                outDriver_.disposeBuffers();
                return ASE_InvalidParameter;
            }
            inChannelActive_[static_cast<size_t>(ch)] = true;
            bufferInfos[i].buffers[0] = hostInBuf0_[static_cast<size_t>(ch)].data();
            bufferInfos[i].buffers[1] = hostInBuf1_[static_cast<size_t>(ch)].data();
        } else {
            if (ch < 0 || ch >= outChTotal) {
                inDriver_.disposeBuffers();
                outDriver_.disposeBuffers();
                return ASE_InvalidParameter;
            }
            outChannelActive_[static_cast<size_t>(ch)] = true;
            // Zero-copy direct output passthrough: hand the output hardware's DMA buffers straight
            // to the DAW like hot potatoes — 0 extra copies and 0 extra output latency.
            bufferInfos[i].buffers[0] = outChildBufs[static_cast<size_t>(ch)].buffers[0];
            bufferInfos[i].buffers[1] = outChildBufs[static_cast<size_t>(ch)].buffers[1];
        }
    }

    // Pre-allocate scratch float buffers for real-time callbacks
    pushScratch_.assign(static_cast<size_t>(inChTotal), std::vector<float>(static_cast<size_t>(inBufSize_), 0.0f));
    pushScratchPtrs_.resize(static_cast<size_t>(inChTotal));
    for (long c = 0; c < inChTotal; ++c) {
        pushScratchPtrs_[static_cast<size_t>(c)] = pushScratch_[static_cast<size_t>(c)].data();
    }

    pullScratch_.assign(static_cast<size_t>(inChTotal), std::vector<float>(static_cast<size_t>(outBufSize_), 0.0f));
    pullScratchPtrs_.resize(static_cast<size_t>(inChTotal));
    for (long c = 0; c < inChTotal; ++c) {
        pullScratchPtrs_[static_cast<size_t>(c)] = pullScratch_[static_cast<size_t>(c)].data();
    }

    // Configure adaptive ring buffer:
    // Because both interfaces burst asynchronously in inBufSize/outBufSize blocks,
    // targetFill needs at least (inBufSize + outBufSize + jitter margin) to survive
    // worst-case phase alignment and Windows USB isochronous jitter with zero underruns.
    const size_t minSafeTarget = static_cast<size_t>(outBufSize_) + static_cast<size_t>(inBufSize_) / 2 + 64;
    size_t targetFill = 0;
    if (config_.ringBufferFrames > 0) {
        targetFill = std::max<size_t>(static_cast<size_t>(config_.ringBufferFrames), minSafeTarget);
    } else {
        size_t maxBuf = static_cast<size_t>(std::max(inBufSize_, outBufSize_));
        targetFill = static_cast<size_t>(inBufSize_) + static_cast<size_t>(outBufSize_) + std::max<size_t>(64, maxBuf / 2);
    }

    ASIOSampleRate sr = currentSampleRate_;
    if (outDriver_.raw()->getSampleRate(&sr) == ASE_OK && sr > 0.0) {
        currentSampleRate_ = sr;
    }
    ringBuffer_.configure(static_cast<size_t>(inChTotal), currentSampleRate_, targetFill, 16384);

    buffersCreated_ = true;
    return ASE_OK;
}

ASIOError BridgeDriver::disposeBuffers() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!buffersCreated_) return ASE_OK;

    streaming_.store(false, std::memory_order_release);
    outDriver_.stop();
    inDriver_.stop();
    outDriver_.disposeBuffers();
    inDriver_.disposeBuffers();

    hostInBuf0_.clear();
    hostInBuf1_.clear();
    pushScratch_.clear();
    pushScratchPtrs_.clear();
    pullScratch_.clear();
    pullScratchPtrs_.clear();
    inChannelActive_.clear();
    outChannelActive_.clear();
    buffersCreated_ = false;
    return ASE_OK;
}

ASIOError BridgeDriver::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_ || !buffersCreated_) return ASE_InvalidMode;

    ringBuffer_.reset();
    totalOutputFrames_.store(0, std::memory_order_relaxed);
    streaming_.store(true, std::memory_order_release);

    // Start input device first so ring buffer begins filling immediately
    ASIOError eIn = inDriver_.start();
    if (eIn != ASE_OK) {
        streaming_.store(false, std::memory_order_release);
        setError("Failed to start input ASIO driver.");
        return eIn;
    }

    ASIOError eOut = outDriver_.start();
    if (eOut != ASE_OK) {
        streaming_.store(false, std::memory_order_release);
        inDriver_.stop();
        setError("Failed to start output ASIO driver.");
        return eOut;
    }

    return ASE_OK;
}

ASIOError BridgeDriver::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    streaming_.store(false, std::memory_order_release);
    if (outDriver_.isOpen()) outDriver_.stop();
    if (inDriver_.isOpen())  inDriver_.stop();
    return ASE_OK;
}

ASIOError BridgeDriver::controlPanel() {
    controlPanel_.show(this, static_cast<HWND>(sysHandle_));
    return ASE_OK;
}

ASIOError BridgeDriver::future(long /*selector*/, void* /*opt*/) {
    return ASE_InvalidParameter;
}

ASIOError BridgeDriver::outputReady() {
    outputReadyCalledByHost_.store(true, std::memory_order_relaxed);
    if (outDriver_.isOpen()) {
        return outDriver_.raw()->outputReady();
    }
    return ASE_NotPresent;
}

// ============================================================================
// ControlPanelHost
// ============================================================================

BridgeConfig BridgeDriver::getConfig() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return config_;
}

void BridgeDriver::applyConfig(const BridgeConfig& cfg) {
    ASIOCallbacks cbs{};
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        config_ = cfg;
        cbs = hostCallbacks_;
    }
    // Ask the DAW to reload our driver so new devices / ring margin take effect cleanly
    if (cbs.asioMessage) {
        cbs.asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
    }
}

void BridgeDriver::openInputDriverPanel() {
    IASIO* drv = nullptr;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        drv = inDriver_.raw();
    }
    if (drv) {
        drv->controlPanel();
    }
}

void BridgeDriver::openOutputDriverPanel() {
    IASIO* drv = nullptr;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        drv = outDriver_.raw();
    }
    if (drv) {
        drv->controlPanel();
    }
}

void BridgeDriver::resetXruns() {
    ringBuffer_.resetXruns();
}

BridgeStatusSnapshot BridgeDriver::getStatus() const {
    BridgeStatusSnapshot snap{};
    snap.streaming = streaming_.load(std::memory_order_relaxed);
    snap.ring = ringBuffer_.getTelemetry();
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        snap.sampleRate = currentSampleRate_;
        snap.inputBufferSize = inBufSize_ > 0 ? inBufSize_ : inDriver_.preferredBufferSize();
        snap.outputBufferSize = outBufSize_ > 0 ? outBufSize_ : outDriver_.preferredBufferSize();
        snap.inputDriverName = inDriver_.driverName();
        snap.outputDriverName = outDriver_.driverName();
    }
    return snap;
}

// ============================================================================
// Child Driver Static Callback Trampolines & Real-Time Audio Path
// ============================================================================
// Because Steinberg's ASIOCallbacks struct in 1997 forgot to include a `void* userData`
// context pointer, we have to bounce through these static C trampolines and an atomic
// singleton pointer (`s_activeInstance_`). To me it's just C callback archaeology but whatevah.
// See: https://en.wikipedia.org/wiki/Trampoline_(computing)

void BridgeDriver::cbInBufferSwitch(long doubleBufferIndex, ASIOBool /*directProcess*/) {
    if (auto* self = s_activeInstance_.load(std::memory_order_acquire)) {
        self->onInputBufferSwitch(doubleBufferIndex);
    }
}

void BridgeDriver::cbInSampleRateDidChange(ASIOSampleRate /*sRate*/) {}

long BridgeDriver::cbInAsioMessage(long selector, long value, void* message, double* opt) {
    if (auto* self = s_activeInstance_.load(std::memory_order_acquire)) {
        return self->onChildAsioMessage(true, selector, value, message, opt);
    }
    return 0;
}

ASIOTime* BridgeDriver::cbInBufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool /*directProcess*/) {
    if (auto* self = s_activeInstance_.load(std::memory_order_acquire)) {
        self->onInputBufferSwitch(doubleBufferIndex);
    }
    return params;
}

void BridgeDriver::cbOutBufferSwitch(long doubleBufferIndex, ASIOBool directProcess) {
    if (auto* self = s_activeInstance_.load(std::memory_order_acquire)) {
        self->onOutputBufferSwitch(nullptr, doubleBufferIndex, directProcess);
    }
}

void BridgeDriver::cbOutSampleRateDidChange(ASIOSampleRate sRate) {
    if (auto* self = s_activeInstance_.load(std::memory_order_acquire)) {
        self->currentSampleRate_ = sRate;
        if (self->hostCallbacks_.sampleRateDidChange) {
            self->hostCallbacks_.sampleRateDidChange(sRate);
        }
    }
}

long BridgeDriver::cbOutAsioMessage(long selector, long value, void* message, double* opt) {
    if (auto* self = s_activeInstance_.load(std::memory_order_acquire)) {
        return self->onChildAsioMessage(false, selector, value, message, opt);
    }
    return 0;
}

ASIOTime* BridgeDriver::cbOutBufferSwitchTimeInfo(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess) {
    if (auto* self = s_activeInstance_.load(std::memory_order_acquire)) {
        return self->onOutputBufferSwitch(params, doubleBufferIndex, directProcess);
    }
    return params;
}

void BridgeDriver::onInputBufferSwitch(long doubleBufferIndex) {
    if (!streaming_.load(std::memory_order_acquire)) return;
    if (doubleBufferIndex != 0 && doubleBufferIndex != 1) return;

    const auto& inBufs = inDriver_.bufferInfos();
    const size_t numCh = std::min(inBufs.size(), pushScratch_.size());
    const size_t frames = static_cast<size_t>(inBufSize_);

    for (size_t c = 0; c < numCh; ++c) {
        const void* src = inBufs[c].buffers[doubleBufferIndex];
        sample_convert::asioToFloat(src, pushScratch_[c].data(), frames, inSampleTypes_[c]);
    }

    ringBuffer_.push(pushScratchPtrs_.data(), frames);
}

ASIOTime* BridgeDriver::onOutputBufferSwitch(ASIOTime* params, long doubleBufferIndex, ASIOBool directProcess) {
    if (!streaming_.load(std::memory_order_acquire)) return params;
    if (doubleBufferIndex != 0 && doubleBufferIndex != 1) return params;

    const size_t frames = static_cast<size_t>(outBufSize_);
    const size_t numInCh = pullScratch_.size();

    // Pull clock-drift-compensated input samples from ring buffer
    ringBuffer_.pull(pullScratchPtrs_.data(), frames);

    // Convert into DAW's input buffers for active channels
    auto& hostBufs = (doubleBufferIndex == 0) ? hostInBuf0_ : hostInBuf1_;
    for (size_t c = 0; c < numInCh; ++c) {
        if (c < inChannelActive_.size() && inChannelActive_[c]) {
            sample_convert::floatToAsio(pullScratch_[c].data(), hostBufs[c].data(), frames, inSampleTypes_[c]);
        }
    }

    outputReadyCalledByHost_.store(false, std::memory_order_relaxed);

    // Drive DAW callback on master (FiiO) clock
    ASIOTime localTime{};
    ASIOTime* result = params;
    if (hostSupportsTimeInfo_ && hostCallbacks_.bufferSwitchTimeInfo) {
        if (!params) {
            uint64_t pos = totalOutputFrames_.load(std::memory_order_relaxed);
            localTime.timeInfo.speed = 1.0;
            localTime.timeInfo.sampleRate = currentSampleRate_;
            localTime.timeInfo.samplePosition = u64ToAsioSamples(pos);
            localTime.timeInfo.flags = kSamplePositionValid | kSampleRateValid | kSpeedValid;
            params = &localTime;
        }
        result = hostCallbacks_.bufferSwitchTimeInfo(params, doubleBufferIndex, directProcess);
    } else if (hostCallbacks_.bufferSwitch) {
        hostCallbacks_.bufferSwitch(doubleBufferIndex, directProcess);
    }

    totalOutputFrames_.fetch_add(frames, std::memory_order_relaxed);

    // Some DAWs politely call outputReady() to tell the hardware "I'm done writing...";
    // others (yes I'm looking at you ChiFi..) just ghost us and return. Cover for the rude ones so the output DAC stays happy.
    if (!outputReadyCalledByHost_.load(std::memory_order_relaxed) && outDriver_.isOpen()) {
        outDriver_.raw()->outputReady();
    }

    return result;
}

long BridgeDriver::onChildAsioMessage(bool /*isInput*/, long selector, long value, void* message, double* opt) {
    switch (selector) {
        case kAsioSelectorSupported:
            switch (value) {
                case kAsioEngineVersion:
                case kAsioResetRequest:
                case kAsioBufferSizeChange:
                case kAsioResyncRequest:
                case kAsioLatenciesChanged:
                case kAsioSupportsTimeInfo:
                    return 1;
                default:
                    return 0;
            }
        case kAsioEngineVersion:
            return 2;
        case kAsioSupportsTimeInfo:
            return 1;
        case kAsioResetRequest:
        case kAsioBufferSizeChange:
        case kAsioResyncRequest:
        case kAsioLatenciesChanged:
            // Forward reset/reconfig request to the DAW
            if (hostCallbacks_.asioMessage) {
                return hostCallbacks_.asioMessage(kAsioResetRequest, 0, message, opt);
            }
            return 1;
        default:
            return 0;
    }
}

void BridgeDriver::setError(const std::string& msg) {
    lastError_ = msg;
}
