#include "shared_tap.h"

#include <algorithm>
#include <cstring>

namespace {
constexpr const wchar_t* kSharedTapMapName   = L"Local\\SplitASIO_Tap_v1";
constexpr const wchar_t* kSharedTapEventName = L"Local\\SplitASIO_Tap_Evt_v1";
} // namespace

// ============================================================================
// SharedTapPublisher
// ============================================================================

SharedTapPublisher::~SharedTapPublisher() {
    close();
}

bool SharedTapPublisher::open(double sampleRate, uint32_t blockFrames) {
    close();

    mapHandle_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(SharedTapBlock)),
        kSharedTapMapName);
    if (!mapHandle_) {
        return false;
    }

    const bool newlyCreated = (GetLastError() != ERROR_ALREADY_EXISTS);
    void* view = MapViewOfFile(mapHandle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedTapBlock));
    if (!view) {
        CloseHandle(mapHandle_);
        mapHandle_ = nullptr;
        return false;
    }

    eventHandle_ = CreateEventW(nullptr, FALSE, FALSE, kSharedTapEventName);
    if (!eventHandle_) {
        UnmapViewOfFile(view);
        CloseHandle(mapHandle_);
        mapHandle_ = nullptr;
        return false;
    }

    block_ = static_cast<SharedTapBlock*>(view);
    if (newlyCreated || block_->magic != SharedTapBlock::kMagic || block_->version != SharedTapBlock::kVersion) {
        std::memset(block_, 0, sizeof(SharedTapBlock));
        block_->magic = SharedTapBlock::kMagic;
        block_->version = SharedTapBlock::kVersion;
    }

    block_->sampleRate.store(sampleRate > 0.0 ? sampleRate : 48000.0, std::memory_order_relaxed);
    block_->blockFrames.store(blockFrames > 0 ? blockFrames : 128u, std::memory_order_relaxed);
    block_->writePos.store(0, std::memory_order_relaxed);
    block_->generation.fetch_add(1, std::memory_order_acq_rel);
    block_->publisherActive.store(1, std::memory_order_release);
    return true;
}

void SharedTapPublisher::close() {
    if (block_) {
        block_->publisherActive.store(0, std::memory_order_release);
        if (eventHandle_) {
            SetEvent(eventHandle_);
        }
        UnmapViewOfFile(block_);
        block_ = nullptr;
    }
    if (eventHandle_) {
        CloseHandle(eventHandle_);
        eventHandle_ = nullptr;
    }
    if (mapHandle_) {
        CloseHandle(mapHandle_);
        mapHandle_ = nullptr;
    }
}

void SharedTapPublisher::push(const float* ch0, const float* ch1, size_t numFrames) {
    if (!block_ || numFrames == 0) return;

    const uint64_t w = block_->writePos.load(std::memory_order_relaxed);
    const float* srcs[2] = { ch0, ch1 };

    for (size_t c = 0; c < 2; ++c) {
        float* dst = block_->samples[c];
        const float* src = srcs[c];
        size_t start = static_cast<size_t>(w & SharedTapBlock::kMask);
        size_t firstPart = std::min(numFrames, SharedTapBlock::kCapacity - start);
        if (src) {
            std::memcpy(dst + start, src, firstPart * sizeof(float));
            if (firstPart < numFrames) {
                std::memcpy(dst, src + firstPart, (numFrames - firstPart) * sizeof(float));
            }
        } else {
            std::memset(dst + start, 0, firstPart * sizeof(float));
            if (firstPart < numFrames) {
                std::memset(dst, 0, (numFrames - firstPart) * sizeof(float));
            }
        }
    }

    block_->writePos.store(w + numFrames, std::memory_order_release);
    if (eventHandle_) {
        SetEvent(eventHandle_);
    }
}

// ============================================================================
// SharedTapSubscriber
// ============================================================================

SharedTapSubscriber::~SharedTapSubscriber() {
    close();
}

bool SharedTapSubscriber::open() {
    close();

    // CreateFileMappingW opens the existing segment if the DAW is already running,
    // or creates a fresh silent one if OBS starts before the DAW.
    mapHandle_ = CreateFileMappingW(
        INVALID_HANDLE_VALUE,
        nullptr,
        PAGE_READWRITE,
        0,
        static_cast<DWORD>(sizeof(SharedTapBlock)),
        kSharedTapMapName);
    if (!mapHandle_) {
        return false;
    }

    const bool newlyCreated = (GetLastError() != ERROR_ALREADY_EXISTS);
    void* view = MapViewOfFile(mapHandle_, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(SharedTapBlock));
    if (!view) {
        CloseHandle(mapHandle_);
        mapHandle_ = nullptr;
        return false;
    }

    eventHandle_ = CreateEventW(nullptr, FALSE, FALSE, kSharedTapEventName);
    if (!eventHandle_) {
        UnmapViewOfFile(view);
        CloseHandle(mapHandle_);
        mapHandle_ = nullptr;
        return false;
    }

    block_ = static_cast<SharedTapBlock*>(view);
    if (newlyCreated || block_->magic != SharedTapBlock::kMagic || block_->version != SharedTapBlock::kVersion) {
        std::memset(block_, 0, sizeof(SharedTapBlock));
        block_->magic = SharedTapBlock::kMagic;
        block_->version = SharedTapBlock::kVersion;
        block_->sampleRate.store(48000.0, std::memory_order_relaxed);
        block_->blockFrames.store(128u, std::memory_order_relaxed);
    }

    resetReadPosition();
    return true;
}

void SharedTapSubscriber::close() {
    if (block_) {
        UnmapViewOfFile(block_);
        block_ = nullptr;
    }
    if (eventHandle_) {
        CloseHandle(eventHandle_);
        eventHandle_ = nullptr;
    }
    if (mapHandle_) {
        CloseHandle(mapHandle_);
        mapHandle_ = nullptr;
    }
    synced_ = false;
}

bool SharedTapSubscriber::isPublisherActive() const {
    return block_ && block_->publisherActive.load(std::memory_order_acquire) != 0;
}

double SharedTapSubscriber::publisherSampleRate(double fallback) const {
    if (!isPublisherActive()) return fallback;
    double sr = block_->sampleRate.load(std::memory_order_relaxed);
    return (sr > 0.0) ? sr : fallback;
}

uint32_t SharedTapSubscriber::publisherBlockFrames(uint32_t fallback) const {
    if (!isPublisherActive()) return fallback;
    uint32_t bf = block_->blockFrames.load(std::memory_order_relaxed);
    return (bf >= 32u && bf <= 2048u) ? bf : fallback;
}

bool SharedTapSubscriber::waitForBlock(DWORD timeoutMs) {
    if (!eventHandle_) {
        Sleep(timeoutMs);
        return false;
    }
    return WaitForSingleObject(eventHandle_, timeoutMs) == WAIT_OBJECT_0;
}

void SharedTapSubscriber::resetReadPosition() {
    synced_ = false;
    readPos_ = 0;
    lastGeneration_ = 0;
}

bool SharedTapSubscriber::pull(float* ch0, float* ch1, size_t numFrames) {
    auto outputSilence = [&]() {
        if (ch0) std::memset(ch0, 0, numFrames * sizeof(float));
        if (ch1) std::memset(ch1, 0, numFrames * sizeof(float));
    };

    if (!block_ || numFrames == 0 || !isPublisherActive()) {
        synced_ = false;
        outputSilence();
        return false;
    }

    const uint32_t gen = block_->generation.load(std::memory_order_acquire);
    const uint64_t w = block_->writePos.load(std::memory_order_acquire);

    if (!synced_ || gen != lastGeneration_) {
        lastGeneration_ = gen;
        if (w < numFrames) {
            outputSilence();
            return false;
        }
        readPos_ = w - numFrames;
        synced_ = true;
    }

    const int64_t avail = static_cast<int64_t>(w) - static_cast<int64_t>(readPos_);
    if (avail < static_cast<int64_t>(numFrames)) {
        // Publisher hasn't produced enough frames yet (e.g. DAW paused)
        outputSilence();
        return false;
    }

    // If subscriber fell way behind (e.g. OBS stalled), snap to the freshest block
    const int64_t maxLag = static_cast<int64_t>(std::max<size_t>(numFrames * 4, 1024));
    if (avail > maxLag) {
        readPos_ = w - numFrames;
    }

    float* dsts[2] = { ch0, ch1 };
    for (size_t c = 0; c < 2; ++c) {
        if (!dsts[c]) continue;
        const float* src = block_->samples[c];
        size_t start = static_cast<size_t>(readPos_ & SharedTapBlock::kMask);
        size_t firstPart = std::min(numFrames, SharedTapBlock::kCapacity - start);
        std::memcpy(dsts[c], src + start, firstPart * sizeof(float));
        if (firstPart < numFrames) {
            std::memcpy(dsts[c] + firstPart, src, (numFrames - firstPart) * sizeof(float));
        }
    }

    readPos_ += numFrames;
    return true;
}
