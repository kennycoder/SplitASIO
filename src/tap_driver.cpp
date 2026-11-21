#include "tap_driver.h"
#include "driver_guid.h"

#include <avrt.h>
#include <chrono>
#include <cmath>
#include <cstring>
#include <windows.h>

extern void dllObjectCreated();
extern void dllObjectDestroyed();

namespace {

uint64_t getSystemTimeNs() {
    LARGE_INTEGER freq{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (freq.QuadPart <= 0) return 0;
    return static_cast<uint64_t>((static_cast<long double>(now.QuadPart) * 1.0e9L) / static_cast<long double>(freq.QuadPart));
}

bool isSupportedRate(double sr) {
    constexpr double kRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 192000.0 };
    for (double r : kRates) {
        if (std::abs(sr - r) < 1.0) return true;
    }
    return false;
}

} // namespace

TapDriver::TapDriver() {
    dllObjectCreated();
}

TapDriver::~TapDriver() {
    stop();
    disposeBuffers();
    subscriber_.close();
    dllObjectDestroyed();
}

// ============================================================================
// IUnknown
// ============================================================================

HRESULT STDMETHODCALLTYPE TapDriver::QueryInterface(REFIID riid, void** ppvObject) {
    if (!ppvObject) return E_POINTER;
    if (IsEqualIID(riid, IID_IUnknown) || IsEqualIID(riid, CLSID_SplitAsioTap)) {
        *ppvObject = static_cast<IASIO*>(this);
        AddRef();
        return S_OK;
    }
    *ppvObject = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE TapDriver::AddRef() {
    return refCount_.fetch_add(1, std::memory_order_relaxed) + 1;
}

ULONG STDMETHODCALLTYPE TapDriver::Release() {
    ULONG count = refCount_.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (count == 0) {
        delete this;
    }
    return count;
}

// ============================================================================
// IASIO
// ============================================================================

ASIOBool TapDriver::init(void* /*sysHandle*/) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!subscriber_.open()) {
        lastError_ = "Failed to open SplitASIO shared-memory tap.";
        return ASIOFalse;
    }
    sampleRate_ = subscriber_.publisherSampleRate(48000.0);
    bufferSize_ = static_cast<long>(subscriber_.publisherBlockFrames(128u));
    initialized_ = true;
    return ASIOTrue;
}

void TapDriver::getDriverName(char* name) {
    if (!name) return;
    strncpy_s(name, 32, kSplitAsioTapNameA, _TRUNCATE);
}

long TapDriver::getDriverVersion() {
    return 101;
}

void TapDriver::getErrorMessage(char* string) {
    if (!string) return;
    std::lock_guard<std::mutex> lock(stateMutex_);
    strncpy_s(string, 124, lastError_.c_str(), _TRUNCATE);
}

ASIOError TapDriver::getChannels(long* numInputChannels, long* numOutputChannels) {
    if (!initialized_) return ASE_NotPresent;
    if (numInputChannels)  *numInputChannels  = 2;
    if (numOutputChannels) *numOutputChannels = 0;
    return ASE_OK;
}

ASIOError TapDriver::getLatencies(long* inputLatency, long* outputLatency) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (inputLatency)  *inputLatency  = bufferSize_;
    if (outputLatency) *outputLatency = 0;
    return ASE_OK;
}

ASIOError TapDriver::getBufferSize(long* minSize, long* maxSize, long* preferredSize, long* granularity) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    long pref = static_cast<long>(subscriber_.publisherBlockFrames(static_cast<uint32_t>(bufferSize_)));
    if (minSize)       *minSize       = 64;
    if (maxSize)       *maxSize       = 2048;
    if (preferredSize) *preferredSize = pref;
    if (granularity)   *granularity   = 64;
    return ASE_OK;
}

ASIOError TapDriver::canSampleRate(ASIOSampleRate sampleRate) {
    if (!initialized_) return ASE_NotPresent;
    return isSupportedRate(sampleRate) ? ASE_OK : ASE_NoClock;
}

ASIOError TapDriver::getSampleRate(ASIOSampleRate* sampleRate) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!sampleRate) return ASE_InvalidParameter;
    if (subscriber_.isPublisherActive()) {
        sampleRate_ = subscriber_.publisherSampleRate(sampleRate_);
    }
    *sampleRate = sampleRate_;
    return ASE_OK;
}

ASIOError TapDriver::setSampleRate(ASIOSampleRate sampleRate) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!isSupportedRate(sampleRate)) return ASE_NoClock;
    sampleRate_ = sampleRate;
    return ASE_OK;
}

ASIOError TapDriver::getClockSources(ASIOClockSource* clocks, long* numSources) {
    if (!clocks || !numSources || *numSources <= 0) return ASE_InvalidParameter;
    clocks[0].index = 0;
    clocks[0].associatedChannel = -1;
    clocks[0].associatedGroup = -1;
    clocks[0].isCurrentSource = ASIOTrue;
    strncpy_s(clocks[0].name, sizeof(clocks[0].name), "SplitASIO Shared Clock", _TRUNCATE);
    *numSources = 1;
    return ASE_OK;
}

ASIOError TapDriver::setClockSource(long reference) {
    return (reference == 0) ? ASE_OK : ASE_InvalidParameter;
}

ASIOError TapDriver::getSamplePosition(ASIOSamples* sPos, ASIOTimeStamp* tStamp) {
    if (!initialized_) return ASE_NotPresent;
    if (sPos)   *sPos   = u64ToAsioSamples(samplePosition_.load(std::memory_order_relaxed));
    if (tStamp) *tStamp = u64ToAsioTimeStamp(getSystemTimeNs());
    return ASE_OK;
}

ASIOError TapDriver::getChannelInfo(ASIOChannelInfo* info) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!info) return ASE_InvalidParameter;
    if (!info->isInput || info->channel < 0 || info->channel >= 2) {
        return ASE_InvalidParameter;
    }

    info->isActive = channelActive_[info->channel] ? ASIOTrue : ASIOFalse;
    info->channelGroup = 0;
    info->type = ASIOSTFloat32LSB;
    strncpy_s(info->name, sizeof(info->name),
        (info->channel == 0) ? "SplitASIO Tap L" : "SplitASIO Tap R", _TRUNCATE);
    return ASE_OK;
}

ASIOError TapDriver::createBuffers(ASIOBufferInfo* bufferInfos, long numChannels, long bufferSize, ASIOCallbacks* callbacks) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_) return ASE_NotPresent;
    if (!bufferInfos || numChannels <= 0 || bufferSize < 16 || bufferSize > 4096 || !callbacks) {
        return ASE_InvalidParameter;
    }

    if (buffersCreated_) {
        streaming_.store(false, std::memory_order_release);
        if (audioThread_.joinable()) audioThread_.join();
        buffersCreated_ = false;
    }

    bufferSize_ = bufferSize;
    hostCallbacks_ = *callbacks;
    hostSupportsTimeInfo_ = false;
    if (hostCallbacks_.asioMessage && hostCallbacks_.bufferSwitchTimeInfo) {
        if (hostCallbacks_.asioMessage(kAsioSelectorSupported, kAsioSupportsTimeInfo, nullptr, nullptr) == 1) {
            hostSupportsTimeInfo_ = (hostCallbacks_.asioMessage(kAsioSupportsTimeInfo, 0, nullptr, nullptr) == 1);
        }
    }

    channelActive_[0] = false;
    channelActive_[1] = false;
    for (int c = 0; c < 2; ++c) {
        buf0_[c].assign(static_cast<size_t>(bufferSize_), 0.0f);
        buf1_[c].assign(static_cast<size_t>(bufferSize_), 0.0f);
    }

    for (long i = 0; i < numChannels; ++i) {
        if (!bufferInfos[i].isInput || bufferInfos[i].channelNum < 0 || bufferInfos[i].channelNum >= 2) {
            return ASE_InvalidParameter;
        }
        long ch = bufferInfos[i].channelNum;
        channelActive_[ch] = true;
        bufferInfos[i].buffers[0] = buf0_[ch].data();
        bufferInfos[i].buffers[1] = buf1_[ch].data();
    }

    buffersCreated_ = true;
    return ASE_OK;
}

ASIOError TapDriver::disposeBuffers() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!buffersCreated_) return ASE_OK;
    streaming_.store(false, std::memory_order_release);
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    for (int c = 0; c < 2; ++c) {
        buf0_[c].clear();
        buf1_[c].clear();
        channelActive_[c] = false;
    }
    buffersCreated_ = false;
    return ASE_OK;
}

ASIOError TapDriver::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!initialized_ || !buffersCreated_) return ASE_InvalidMode;

    if (audioThread_.joinable()) {
        streaming_.store(false, std::memory_order_release);
        audioThread_.join();
    }

    subscriber_.resetReadPosition();
    samplePosition_.store(0, std::memory_order_relaxed);
    streaming_.store(true, std::memory_order_release);
    audioThread_ = std::thread(&TapDriver::audioThreadMain, this);
    return ASE_OK;
}

ASIOError TapDriver::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    streaming_.store(false, std::memory_order_release);
    if (audioThread_.joinable()) {
        audioThread_.join();
    }
    return ASE_OK;
}

ASIOError TapDriver::controlPanel() {
    MessageBoxW(nullptr,
        L"SplitASIO Tap is a receive-only virtual ASIO input for SplitASIO.\n\n"
        L"To configure routing (Mirror Main Out 1/2 or Separate Aux Bus Out 3/4), open\n"
        L"the main SplitASIO Control Panel from your DAW or Start Menu -> SplitASIO Settings.",
        L"SplitASIO Tap", MB_OK | MB_ICONINFORMATION);
    return ASE_OK;
}

ASIOError TapDriver::future(long /*selector*/, void* /*opt*/) {
    return ASE_InvalidParameter;
}

ASIOError TapDriver::outputReady() {
    return ASE_OK;
}

void TapDriver::audioThreadMain() {
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

    long dbIndex = 0;
    const size_t frames = static_cast<size_t>(bufferSize_);
    const double sr = (sampleRate_ > 0.0) ? sampleRate_ : 48000.0;
    const DWORD fallbackMs = std::max<DWORD>(2u, static_cast<DWORD>((static_cast<double>(frames) * 1000.0 / sr) + 1.5));

    while (streaming_.load(std::memory_order_acquire)) {
        // Wait for SplitASIO's master clock event; if the DAW isn't streaming yet,
        // pace cleanly via timeout and emit silence so OBS never stalls or disconnects.
        subscriber_.waitForBlock(fallbackMs);
        if (!streaming_.load(std::memory_order_acquire)) break;

        float* ch0 = (dbIndex == 0) ? buf0_[0].data() : buf1_[0].data();
        float* ch1 = (dbIndex == 0) ? buf0_[1].data() : buf1_[1].data();
        subscriber_.pull(ch0, ch1, frames);

        const uint64_t pos = samplePosition_.fetch_add(frames, std::memory_order_relaxed);
        if (hostSupportsTimeInfo_ && hostCallbacks_.bufferSwitchTimeInfo) {
            ASIOTime t{};
            t.timeInfo.speed = 1.0;
            t.timeInfo.sampleRate = sr;
            t.timeInfo.samplePosition = u64ToAsioSamples(pos);
            t.timeInfo.systemTime = u64ToAsioTimeStamp(getSystemTimeNs());
            t.timeInfo.flags = kSystemTimeValid | kSamplePositionValid | kSampleRateValid | kSpeedValid;
            hostCallbacks_.bufferSwitchTimeInfo(&t, dbIndex, ASIOTrue);
        } else if (hostCallbacks_.bufferSwitch) {
            hostCallbacks_.bufferSwitch(dbIndex, ASIOTrue);
        }

        dbIndex ^= 1;
    }

    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
}
