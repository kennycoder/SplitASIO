#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <windows.h>

// Cross-process lock-free stereo audio ring buffer in Windows named shared memory.
// Lets SplitASIO (in the DAW process) stream processed audio over pure ASIO-to-ASIO IPC
// to "SplitASIO Tap" (in OBS obs-asio or a second DAW) without virtual cables or kernel drivers.
// Docs: https://learn.microsoft.com/en-us/windows/win32/memory/creating-named-shared-memory

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

struct SharedTapBlock {
    static constexpr uint32_t kMagic    = 0x53504C54u; // "SPLT"
    static constexpr uint32_t kVersion  = 1u;
    static constexpr size_t   kCapacity = 16384u;      // Power of 2 (~341 ms @ 48 kHz)
    static constexpr size_t   kMask     = kCapacity - 1u;

    uint32_t magic;
    uint32_t version;
    std::atomic<uint32_t> publisherActive;
    std::atomic<uint32_t> generation;
    std::atomic<double>   sampleRate;
    std::atomic<uint32_t> blockFrames;

    alignas(64) std::atomic<uint64_t> writePos;
    alignas(64) float samples[2][kCapacity];
};

#ifdef _MSC_VER
#pragma warning(pop)
#endif

class SharedTapPublisher {
public:
    SharedTapPublisher() = default;
    ~SharedTapPublisher();

    SharedTapPublisher(const SharedTapPublisher&) = delete;
    SharedTapPublisher& operator=(const SharedTapPublisher&) = delete;

    bool open(double sampleRate, uint32_t blockFrames);
    void close();
    bool isOpen() const { return block_ != nullptr; }

    // Real-time safe (lock-free, zero allocations)
    void push(const float* ch0, const float* ch1, size_t numFrames);

private:
    HANDLE mapHandle_ = nullptr;
    HANDLE eventHandle_ = nullptr;
    SharedTapBlock* block_ = nullptr;
};

class SharedTapSubscriber {
public:
    SharedTapSubscriber() = default;
    ~SharedTapSubscriber();

    SharedTapSubscriber(const SharedTapSubscriber&) = delete;
    SharedTapSubscriber& operator=(const SharedTapSubscriber&) = delete;

    bool open();
    void close();
    bool isOpen() const { return block_ != nullptr; }

    // Queries publisher metadata (falls back to defaults if publisher is idle)
    bool isPublisherActive() const;
    double publisherSampleRate(double fallback = 48000.0) const;
    uint32_t publisherBlockFrames(uint32_t fallback = 128u) const;

    // Waits up to timeoutMs for the publisher to signal a new block.
    // Returns true if woken by publisher, false on timeout.
    bool waitForBlock(DWORD timeoutMs);

    // Reads numFrames of stereo float audio into ch0/ch1 (outputs silence if publisher is idle).
    bool pull(float* ch0, float* ch1, size_t numFrames);

    void resetReadPosition();

private:
    HANDLE mapHandle_ = nullptr;
    HANDLE eventHandle_ = nullptr;
    SharedTapBlock* block_ = nullptr;

    uint32_t lastGeneration_ = 0;
    uint64_t readPos_ = 0;
    bool synced_ = false;
};
