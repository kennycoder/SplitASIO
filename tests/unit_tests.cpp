#include "asio_abi.h"
#include "driver_guid.h"
#include "ring_buffer.h"
#include "sample_convert.h"
#include "shared_tap.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <objbase.h>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

int g_failures = 0;

#define CHECK(cond, msg)                                                      \
    do {                                                                      \
        if (!(cond)) {                                                        \
            std::printf("  [FAIL] %s (line %d)\n", (msg), __LINE__);          \
            ++g_failures;                                                     \
            return;                                                           \
        }                                                                     \
    } while (0)

constexpr double kTwoPi = 6.28318530717958647692;

// 1. Sample format conversion round-trips & sign-extension tests
void testSampleConvert() {
    std::printf("[TEST] sample_convert round-trips...\n");

    const std::vector<float> input = {
        -1.0f, -0.75f, -0.5f, -0.001f, 0.0f, 0.001f, 0.5f, 0.75f, 0.999f
    };

    struct FormatCase {
        ASIOSampleType type;
        float tolerance;
    };
    const FormatCase cases[] = {
        { ASIOSTInt32LSB,   1e-6f },
        { ASIOSTFloat32LSB, 1e-7f },
        { ASIOSTFloat64LSB, 1e-6f },
        { ASIOSTInt24LSB,   1e-5f },
        { ASIOSTInt32LSB24, 1e-5f },
        { ASIOSTInt16LSB,   1e-4f },
        { ASIOSTInt32LSB16, 1e-4f },
    };

    for (const auto& fc : cases) {
        std::vector<uint8_t> raw(input.size() * sample_convert::sampleByteSize(fc.type), 0);
        std::vector<float> decoded(input.size(), 0.0f);

        sample_convert::floatToAsio(input.data(), raw.data(), input.size(), fc.type);
        sample_convert::asioToFloat(raw.data(), decoded.data(), input.size(), fc.type);

        for (size_t i = 0; i < input.size(); ++i) {
            float err = std::abs(decoded[i] - input[i]);
            if (err > fc.tolerance) {
                std::printf("  [FAIL] format=%s idx=%zu in=%.6f out=%.6f err=%.6f\n",
                    asioSampleTypeName(fc.type), i, input[i], decoded[i], err);
                ++g_failures;
                return;
            }
        }
    }

    // Verify clamping for out-of-range floats (> +1.0 / < -1.0)
    const float extreme[2] = { -2.5f, 2.5f };
    int32_t outI32[2]{};
    sample_convert::floatToAsio(extreme, outI32, 2, ASIOSTInt32LSB);
    CHECK(outI32[0] <= -2147483640, "Negative overflow should clamp near INT32_MIN");
    CHECK(outI32[1] >= 2147483640, "Positive overflow should clamp near INT32_MAX");

    std::printf("  [PASS]\n");
}

// 2. Adaptive ring buffer clock-drift soak test (+65 ppm and -65 ppm)
void runDriftSoakCase(double driftPpm) {
    std::printf("[TEST] AdaptiveAudioRingBuffer drift soak (%+.0f ppm, 120s virtual)...\n", driftPpm);

    constexpr double kSampleRate = 48000.0;
    constexpr size_t kBlock = 128;
    constexpr size_t kTargetFill = 320; // Same as Auto for 128/128
    constexpr double kSimSeconds = 120.0;

    AdaptiveAudioRingBuffer rb;
    rb.configure(2, kSampleRate, kTargetFill, 16384);

    // Input runs at (1 + driftPpm * 1e-6) relative to output clock
    const double inBlocksPerOutBlock = 1.0 + (driftPpm * 1.0e-6);
    const size_t totalOutBlocks = static_cast<size_t>((kSimSeconds * kSampleRate) / kBlock);

    std::vector<float> inL(kBlock), inR(kBlock);
    std::vector<float> outL(kBlock), outR(kBlock);
    const float* inPtrs[2] = { inL.data(), inR.data() };
    float* outPtrs[2] = { outL.data(), outR.data() };

    double inPhase = 0.0;
    double inBlockAccum = 3.0; // Start slightly ahead just like starting input before output
    double maxOutPeak = 0.0;

    auto pushOneInputBlock = [&]() {
        for (size_t i = 0; i < kBlock; ++i) {
            float s = 0.5f * static_cast<float>(std::sin(inPhase));
            inL[i] = s;
            inR[i] = -s;
            inPhase += kTwoPi * 440.0 / kSampleRate;
        }
        rb.push(inPtrs, kBlock);
    };

    for (size_t b = 0; b < totalOutBlocks; ++b) {
        inBlockAccum += inBlocksPerOutBlock;
        while (inBlockAccum >= 1.0) {
            pushOneInputBlock();
            inBlockAccum -= 1.0;
        }

        rb.pull(outPtrs, kBlock);
        if (b > 100) {
            for (size_t i = 0; i < kBlock; ++i) {
                double v = std::abs(outL[i]);
                if (v > maxOutPeak) maxOutPeak = v;
            }
        }
    }

    auto tel = rb.getTelemetry();
    std::printf("  Result: drift=%+.2f ppm (true %+.1f), fill=%.1f/%.0f, xruns=%llu/%llu, peak=%.4f\n",
        tel.driftPpm, driftPpm, tel.fillFrames, tel.targetFillFrames,
        static_cast<unsigned long long>(tel.underruns),
        static_cast<unsigned long long>(tel.overruns),
        maxOutPeak);

    CHECK(tel.primed, "Ring buffer must be primed");
    CHECK(tel.underruns == 0, "Expected 0 underruns during clock-drift soak");
    CHECK(tel.overruns == 0, "Expected 0 overruns during clock-drift soak");
    CHECK(std::abs(tel.fillFrames - static_cast<double>(kTargetFill)) < 32.0,
          "PI controller should hold fill level near target after absorbing 374+ drift frames");
    CHECK(std::abs(tel.driftPpm) < 500.0, "Drift compensation should remain stable");
    CHECK(maxOutPeak > 0.48 && maxOutPeak < 0.52, "Resampled sine wave peak should be preserved");

    std::printf("  [PASS]\n");
}

// 3. Regression test: underrun re-priming must wait for fresh frames (no infinite underrun loop)
void testUnderrunReprime() {
    std::printf("[TEST] AdaptiveAudioRingBuffer underrun recovery...\n");

    AdaptiveAudioRingBuffer rb;
    rb.configure(1, 48000.0, 320, 16384);

    std::vector<float> inBuf(128, 0.25f);
    std::vector<float> outBuf(128, 0.0f);
    const float* inPtr[1] = { inBuf.data() };
    float* outPtr[1] = { outBuf.data() };

    // Warm up past initial priming (push 20 blocks, pull 15)
    for (int i = 0; i < 5; ++i) rb.push(inPtr, 128);
    for (int i = 0; i < 15; ++i) {
        rb.push(inPtr, 128);
        rb.pull(outPtr, 128);
    }
    CHECK(rb.getTelemetry().underruns == 0, "Should have 0 underruns before stall");

    // Simulate a severe input stall (pull 10 times with no pushes -> forces underrun)
    for (int i = 0; i < 10; ++i) {
        rb.pull(outPtr, 128);
    }
    uint64_t xrunsAfterStall = rb.getTelemetry().underruns;
    CHECK(xrunsAfterStall >= 1, "Stall should record an underrun");

    // Resume normal 1:1 streaming; should re-prime cleanly and NOT accumulate further underruns
    for (int i = 0; i < 50; ++i) {
        rb.push(inPtr, 128);
        rb.pull(outPtr, 128);
    }

    auto tel = rb.getTelemetry();
    CHECK(tel.primed, "Should re-prime cleanly after input resumes");
    CHECK(tel.underruns == xrunsAfterStall, "Must not keep underrunning after re-prime");
    CHECK(std::abs(outBuf[64] - 0.25f) < 0.01f, "Audio should recover cleanly after re-prime");

    std::printf("  [PASS]\n");
}

// 4. Regression test: WASAPI large consumer pulls (20 ms / 960 frames) vs 128-frame ASIO pushes
void testWasapiLargePullChunking() {
    std::printf("[TEST] WASAPI large-chunk pull stability...\n");

    constexpr double kSr = 48000.0;
    constexpr size_t kAsioBlock = 128;
    constexpr size_t kTargetFill = static_cast<size_t>(kSr * 0.045); // 2160 frames (matches WasapiSender)
    constexpr size_t kWasapiPull = 960;                              // 20 ms WASAPI chunk

    AdaptiveAudioRingBuffer rb;
    rb.configure(2, kSr, kTargetFill, 32768);

    std::vector<float> inL(kAsioBlock, 0.4f), inR(kAsioBlock, -0.4f);
    std::vector<float> outL(kWasapiPull, 0.0f), outR(kWasapiPull, 0.0f);
    const float* inPtrs[2] = { inL.data(), inR.data() };
    float* outPtrs[2] = { outL.data(), outR.data() };

    // Simulate 5 seconds of 128-frame pushes + 960-frame WASAPI pulls
    size_t pushed = 0;
    size_t pulled = 0;
    while (pulled < 48000 * 5) {
        while (pushed < pulled + kTargetFill + kWasapiPull) {
            rb.push(inPtrs, kAsioBlock);
            pushed += kAsioBlock;
        }
        rb.pull(outPtrs, kWasapiPull);
        pulled += kWasapiPull;
    }

    auto tel = rb.getTelemetry();
    CHECK(tel.primed, "WASAPI ring must stay primed");
    CHECK(tel.underruns == 0, "WASAPI ring must have 0 underruns");
    CHECK(tel.overruns == 0, "WASAPI ring must have 0 overruns");
    CHECK(std::abs(outL[100] - 0.4f) < 0.01f, "WASAPI output must contain live audio (not silence)");

    std::printf("  [PASS]\n");
}

// 5. Shared-memory IPC + SplitASIO Tap COM driver test
std::atomic<uint64_t> g_tapCbs{0};
std::atomic<double> g_tapPeak{0.0};
ASIOBufferInfo g_tapBufs[2]{};

void cbTapSwitch(long idx, ASIOBool) {
    g_tapCbs.fetch_add(1, std::memory_order_relaxed);
    const auto* s = static_cast<const float*>(g_tapBufs[0].buffers[idx]);
    if (!s) return;
    double localMax = 0.0;
    for (int i = 0; i < 128; ++i) {
        double v = std::abs(s[i]);
        if (v > localMax) localMax = v;
    }
    double prev = g_tapPeak.load(std::memory_order_relaxed);
    while (localMax > prev && !g_tapPeak.compare_exchange_weak(prev, localMax, std::memory_order_relaxed)) {}
}
void cbTapSrChange(ASIOSampleRate) {}
long cbTapMsg(long sel, long val, void*, double*) {
    if (sel == kAsioSelectorSupported) return (val == kAsioEngineVersion || val == kAsioSupportsTimeInfo) ? 1 : 0;
    if (sel == kAsioEngineVersion) return 2;
    if (sel == kAsioSupportsTimeInfo) return 1;
    return 0;
}
ASIOTime* cbTapTimeInfo(ASIOTime* p, long idx, ASIOBool dp) {
    cbTapSwitch(idx, dp);
    return p;
}

void testTapDriverComAndIpc() {
    std::printf("[TEST] SplitASIO Tap COM + Shared-Memory IPC...\n");

    using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    using DllCanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

    HMODULE dll = LoadLibraryW(L"split_asio.dll");
    CHECK(dll != nullptr, "Failed to load split_asio.dll");

    auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    auto canUnloadNow = reinterpret_cast<DllCanUnloadNowFn>(GetProcAddress(dll, "DllCanUnloadNow"));
    CHECK(getClassObject != nullptr && canUnloadNow != nullptr, "Missing COM exports");

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_SplitAsioTap, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    CHECK(SUCCEEDED(hr) && factory != nullptr, "DllGetClassObject(CLSID_SplitAsioTap) failed");

    IASIO* tap = nullptr;
    hr = factory->CreateInstance(nullptr, CLSID_SplitAsioTap, reinterpret_cast<void**>(&tap));
    factory->Release();
    CHECK(SUCCEEDED(hr) && tap != nullptr, "CreateInstance(TapDriver) failed");

    CHECK(tap->init(GetDesktopWindow()) == ASIOTrue, "TapDriver::init() failed");

    long inCh = 0, outCh = 0;
    CHECK(tap->getChannels(&inCh, &outCh) == ASE_OK && inCh == 2 && outCh == 0, "Expected 2 in / 0 out");

    g_tapBufs[0] = ASIOBufferInfo{ ASIOTrue, 0, { nullptr, nullptr } };
    g_tapBufs[1] = ASIOBufferInfo{ ASIOTrue, 1, { nullptr, nullptr } };
    ASIOCallbacks cbs{ cbTapSwitch, cbTapSrChange, cbTapMsg, cbTapTimeInfo };
    CHECK(tap->createBuffers(g_tapBufs, 2, 128, &cbs) == ASE_OK, "TapDriver::createBuffers() failed");

    SharedTapPublisher pub;
    CHECK(pub.open(48000.0, 128), "SharedTapPublisher::open() failed");

    g_tapCbs.store(0);
    g_tapPeak.store(0.0);
    CHECK(tap->start() == ASE_OK, "TapDriver::start() failed");

    std::vector<float> sine(128, 0.0f);
    double ph = 0.0;
    for (int b = 0; b < 40; ++b) {
        for (int i = 0; i < 128; ++i) {
            sine[static_cast<size_t>(i)] = 0.6f * static_cast<float>(std::sin(ph));
            ph += kTwoPi * 1000.0 / 48000.0;
        }
        pub.push(sine.data(), sine.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    tap->stop();
    pub.close();
    tap->disposeBuffers();
    tap->Release();

    CHECK(g_tapCbs.load() >= 10, "Expected Tap callbacks to fire");
    CHECK(g_tapPeak.load() > 0.55 && g_tapPeak.load() < 0.65, "Expected 0.6 sine wave peak over IPC");
    CHECK(canUnloadNow() == S_OK, "DllCanUnloadNow() must return S_OK (no COM leaks)");

    FreeLibrary(dll);
    std::printf("  [PASS]\n");
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    testSampleConvert();
    runDriftSoakCase(+65.0);
    runDriftSoakCase(-65.0);
    testUnderrunReprime();
    testWasapiLargePullChunking();
    testTapDriverComAndIpc();

    CoUninitialize();

    if (g_failures == 0) {
        std::printf("\nALL UNIT TESTS PASSED!\n");
        return 0;
    }
    std::printf("\n%d TEST(S) FAILED!\n", g_failures);
    return 1;
}
