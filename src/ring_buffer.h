#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

// Lock-free SPSC multi-channel audio ring buffer with PI-controlled
// adaptive cubic Hermite (Catmull-Rom) resampling for independent ASIO clocks.
//
// Why does this exist? Because two USB audio interfaces each have their own cheap
// quartz crystal oscillator, and both are boldly lying when they say "48,000 Hz".
// One might be 48,000.9 Hz and the other 47,999.1 Hz. Without adaptive resampling,
// a naive FIFO will inevitably underrun or overrun and ruin your guitar solo.
// See: https://en.wikipedia.org/wiki/Clock_drift
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4324) // MSVC whining that alignas(64) added padding — yes, that was the entire point.
#endif
class AdaptiveAudioRingBuffer {
public:
    struct Telemetry {
        double driftPpm = 0.0;
        double fillFrames = 0.0;
        double targetFillFrames = 0.0;
        uint64_t underruns = 0;
        uint64_t overruns = 0;
        bool primed = false;
    };

    void configure(size_t numChannels, double sampleRate, size_t targetFillFrames, size_t capacityPowerOf2 = 16384) {
        numChannels_ = numChannels;
        sampleRate_ = (sampleRate > 0.0) ? sampleRate : 48000.0;
        capacity_ = nextPowerOf2(std::max<size_t>(capacityPowerOf2, 4096));
        mask_ = capacity_ - 1;
        targetFill_ = static_cast<double>(std::clamp<size_t>(targetFillFrames, 32, capacity_ / 4));

        channels_.assign(numChannels_, std::vector<float>(capacity_, 0.0f));
        reset();
    }

    void reset() {
        for (auto& ch : channels_) {
            std::fill(ch.begin(), ch.end(), 0.0f);
        }
        writeIndex_.store(0, std::memory_order_relaxed);
        readIndex_ = 0;
        frac_ = 0.0;
        primed_ = false;
        fadeInSamples_ = 0;
        smoothedFill_ = targetFill_;
        integralError_ = 0.0;
        currentRatio_ = 1.0;

        driftPpm_.store(0.0, std::memory_order_relaxed);
        fillFrames_.store(0.0, std::memory_order_relaxed);
        targetFillAtomic_.store(targetFill_, std::memory_order_relaxed);
        primedAtomic_.store(false, std::memory_order_relaxed);
    }

    void resetXruns() {
        underruns_.store(0, std::memory_order_relaxed);
        overruns_.store(0, std::memory_order_relaxed);
    }

    // Producer thread (Input ASIO callback): writes `numFrames` per channel.
    // `channelPtrs` must have at least `numChannels_` pointers.
    void push(const float* const* channelPtrs, size_t numFrames) {
        if (numChannels_ == 0 || numFrames == 0) return;

        uint64_t w = writeIndex_.load(std::memory_order_relaxed);
        for (size_t ch = 0; ch < numChannels_; ++ch) {
            const float* src = channelPtrs[ch];
            float* dst = channels_[ch].data();
            if (!src) {
                for (size_t i = 0; i < numFrames; ++i) {
                    dst[(w + i) & mask_] = 0.0f;
                }
            } else {
                size_t start = static_cast<size_t>(w & mask_);
                size_t firstPart = std::min(numFrames, capacity_ - start);
                std::memcpy(dst + start, src, firstPart * sizeof(float));
                if (firstPart < numFrames) {
                    std::memcpy(dst, src + firstPart, (numFrames - firstPart) * sizeof(float));
                }
            }
        }
        // Release-publish the new writeIndex AFTER sample writes finish so the consumer
        // thread cannot read half-baked audio from the future.
        // Docs: https://en.cppreference.com/w/cpp/atomic/memory_order#Release-Acquire_ordering
        writeIndex_.store(w + numFrames, std::memory_order_release);
    }

    // Consumer thread (Output ASIO callback): reads `numFrames` resampled frames per channel.
    void pull(float* const* channelPtrs, size_t numFrames) {
        if (numChannels_ == 0 || numFrames == 0) return;

        auto outputSilence = [&]() {
            for (size_t ch = 0; ch < numChannels_; ++ch) {
                if (channelPtrs[ch]) {
                    std::memset(channelPtrs[ch], 0, numFrames * sizeof(float));
                }
            }
        };

        const uint64_t w = writeIndex_.load(std::memory_order_acquire);

        // Priming: politely stall with silence until the input device actually wakes up
        // and fills enough headroom for us to start reading without immediately starving.
        if (!primed_) {
            const uint64_t minPrime = static_cast<uint64_t>(targetFill_) + numFrames + 4;
            if (w < minPrime) {
                fillFrames_.store(static_cast<double>(w), std::memory_order_relaxed);
                outputSilence();
                return;
            }
            readIndex_ = w - static_cast<uint64_t>(targetFill_);
            frac_ = 0.0;
            smoothedFill_ = targetFill_;
            integralError_ = 0.0;
            currentRatio_ = 1.0;
            primed_ = true;
            fadeInSamples_ = 32; // Tiny fade-in so your monitors don't punch you in the face with a DC pop
            primedAtomic_.store(true, std::memory_order_relaxed);
        }

        // Check for extreme overrun or underrun (e.g. USB hiccup, DAW freeze, or cosmic ray)
        const int64_t signedAvail = static_cast<int64_t>(w) - static_cast<int64_t>(readIndex_);
        const double avail = static_cast<double>(signedAvail) - frac_;
        const double maxNeeded = static_cast<double>(numFrames) * 1.01 + 4.0;
        const double maxAllowed = static_cast<double>(capacity_) - static_cast<double>(numFrames) * 2.0 - 64.0;

        if (avail < maxNeeded) {
            // Underrun: input fell behind — throw away our dignity, output silence, and re-prime.
            underruns_.fetch_add(1, std::memory_order_relaxed);
            primed_ = false;
            primedAtomic_.store(false, std::memory_order_relaxed);
            fillFrames_.store(std::max(0.0, avail), std::memory_order_relaxed);
            outputSilence();
            return;
        }

        if (avail > maxAllowed) {
            // Overrun: input ran miles ahead — teleport readIndex back to targetFill with a soft fade-in.
            overruns_.fetch_add(1, std::memory_order_relaxed);
            readIndex_ = w - static_cast<uint64_t>(targetFill_);
            frac_ = 0.0;
            smoothedFill_ = targetFill_;
            fadeInSamples_ = 32;
        }

        // PI clock-drift controller:
        // First smooth the fill level with a 0.5s EMA so normal USB packet jitter doesn't
        // turn your guitar into a seasick vibrato pedal. Then gently nudge the resampling
        // ratio by a few dozen ppm (completely inaudible — < 0.2 cents) to lock onto the
        // true hardware clock ratio indefinitely.
        // See: https://en.wikipedia.org/wiki/Proportional%E2%80%93integral%E2%80%93derivative_controller
        //      https://en.wikipedia.org/wiki/Parts-per_notation
        const double currentAvail = static_cast<double>(static_cast<int64_t>(w) - static_cast<int64_t>(readIndex_)) - frac_;
        const double dt = static_cast<double>(numFrames) / sampleRate_;
        const double alpha = 1.0 - std::exp(-dt / 0.5); // 0.5s smoothing time constant
        smoothedFill_ += alpha * (currentAvail - smoothedFill_);

        const double error = smoothedFill_ - targetFill_;
        const double kp = 1.0 / (sampleRate_ * 1.5);
        const double ki = 1.0 / (sampleRate_ * 20.0);

        integralError_ = std::clamp(integralError_ + error * dt, -sampleRate_ * 0.1, sampleRate_ * 0.1);
        const double ratio = std::clamp(1.0 + kp * error + ki * integralError_, 0.995, 1.005);
        currentRatio_ = ratio;

        // 4-point Catmull-Rom cubic Hermite interpolation across y[-1], y[0], y[1], y[2].
        // Much cleaner high-frequency response than linear interpolation, and cheap enough
        // to run on a real-time audio thread without breaking a sweat.
        // Note: we keep readIndex_ (uint64_t) separate from frac_ (double in [0, 1)) so
        // floating-point precision doesn't degrade even if you leave your DAW open for a year.
        // See: https://en.wikipedia.org/wiki/Cubic_Hermite_spline#Catmull%E2%80%93Rom_spline
        uint64_t baseRead = readIndex_;
        double baseFrac = frac_;

        for (size_t ch = 0; ch < numChannels_; ++ch) {
            float* dst = channelPtrs[ch];
            if (!dst) continue;

            const float* src = channels_[ch].data();
            uint64_t idx = baseRead;
            double f = baseFrac;
            int fade = fadeInSamples_;

            for (size_t i = 0; i < numFrames; ++i) {
                const float y0 = src[(idx - 1) & mask_];
                const float y1 = src[idx & mask_];
                const float y2 = src[(idx + 1) & mask_];
                const float y3 = src[(idx + 2) & mask_];

                const float t = static_cast<float>(f);
                const float a = -0.5f * y0 + 1.5f * y1 - 1.5f * y2 + 0.5f * y3;
                const float b = y0 - 2.5f * y1 + 2.0f * y2 - 0.5f * y3;
                const float c = -0.5f * y0 + 0.5f * y2;
                const float d = y1;

                float sample = ((a * t + b) * t + c) * t + d;

                if (fade > 0) {
                    float gain = static_cast<float>(32 - fade + 1) / 32.0f;
                    sample *= gain;
                    --fade;
                }
                dst[i] = sample;

                f += ratio;
                const uint64_t adv = static_cast<uint64_t>(f);
                idx += adv;
                f -= static_cast<double>(adv);
            }
        }

        // Advance shared consumer position
        const double totalAdv = baseFrac + ratio * static_cast<double>(numFrames);
        const uint64_t wholeAdv = static_cast<uint64_t>(totalAdv);
        readIndex_ = baseRead + wholeAdv;
        frac_ = totalAdv - static_cast<double>(wholeAdv);
        if (fadeInSamples_ > 0) {
            fadeInSamples_ = std::max(0, fadeInSamples_ - static_cast<int>(numFrames));
        }

        driftPpm_.store((ratio - 1.0) * 1.0e6, std::memory_order_relaxed);
        fillFrames_.store(smoothedFill_, std::memory_order_relaxed);
    }

    Telemetry getTelemetry() const {
        Telemetry t{};
        t.driftPpm = driftPpm_.load(std::memory_order_relaxed);
        t.fillFrames = fillFrames_.load(std::memory_order_relaxed);
        t.targetFillFrames = targetFillAtomic_.load(std::memory_order_relaxed);
        t.underruns = underruns_.load(std::memory_order_relaxed);
        t.overruns = overruns_.load(std::memory_order_relaxed);
        t.primed = primedAtomic_.load(std::memory_order_relaxed);
        return t;
    }

    double targetFillFrames() const { return targetFill_; }

private:
    static size_t nextPowerOf2(size_t v) {
        size_t p = 1;
        while (p < v) p <<= 1;
        return p;
    }

    size_t numChannels_ = 0;
    size_t capacity_ = 16384;
    size_t mask_ = 16383;
    double sampleRate_ = 48000.0;
    double targetFill_ = 192.0;

    std::vector<std::vector<float>> channels_;

    // Keep producer (writeIndex_) and consumer (readIndex_) on separate 64-byte CPU cache lines
    // so the two audio threads don't play ping-pong with L1 cache coherency (false sharing).
    // See: https://en.wikipedia.org/wiki/False_sharing
    alignas(64) std::atomic<uint64_t> writeIndex_{0};
    alignas(64) uint64_t readIndex_ = 0;
    double frac_ = 0.0;
    bool primed_ = false;
    int fadeInSamples_ = 0;
    double smoothedFill_ = 192.0;
    double integralError_ = 0.0;
    double currentRatio_ = 1.0;

    std::atomic<double> driftPpm_{0.0};
    std::atomic<double> fillFrames_{0.0};
    std::atomic<double> targetFillAtomic_{192.0};
    std::atomic<uint64_t> underruns_{0};
    std::atomic<uint64_t> overruns_{0};
    std::atomic<bool> primedAtomic_{false};
};
#ifdef _MSC_VER
#pragma warning(pop)
#endif
