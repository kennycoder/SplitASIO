#include "../asio_abi.h"
#include "../driver_guid.h"
#include "../shared_tap.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <objbase.h>
#include <thread>
#include <vector>
#include <windows.h>

namespace {

std::atomic<uint64_t> g_callbacks{0};
std::atomic<double> g_peakIn0{0.0};
std::atomic<double> g_peakIn1{0.0};
IASIO* g_driver = nullptr;
std::vector<ASIOBufferInfo> g_bufs;
long g_bufSize = 0;

std::atomic<uint64_t> g_tapCallbacks{0};
std::atomic<double> g_tapPeakL{0.0};
std::atomic<double> g_tapPeakR{0.0};
std::vector<ASIOBufferInfo> g_tapBufs;
long g_tapBufSize = 0;

void updatePeakI32(std::atomic<double>& peak, const void* buf, long frames) {
    if (!buf) return;
    const auto* s = static_cast<const int32_t*>(buf);
    double localMax = 0.0;
    for (long i = 0; i < frames; ++i) {
        double v = std::abs(static_cast<double>(s[i]) / 2147483648.0);
        if (v > localMax) localMax = v;
    }
    double prev = peak.load(std::memory_order_relaxed);
    while (localMax > prev && !peak.compare_exchange_weak(prev, localMax, std::memory_order_relaxed)) {}
}

void updatePeakF32(std::atomic<double>& peak, const void* buf, long frames) {
    if (!buf) return;
    const auto* s = static_cast<const float*>(buf);
    double localMax = 0.0;
    for (long i = 0; i < frames; ++i) {
        double v = std::abs(static_cast<double>(s[i]));
        if (v > localMax) localMax = v;
    }
    double prev = peak.load(std::memory_order_relaxed);
    while (localMax > prev && !peak.compare_exchange_weak(prev, localMax, std::memory_order_relaxed)) {}
}

void cbBufferSwitch(long index, ASIOBool) {
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (g_bufs.size() >= 2) {
        updatePeakI32(g_peakIn0, g_bufs[0].buffers[index], g_bufSize);
        updatePeakI32(g_peakIn1, g_bufs[1].buffers[index], g_bufSize);
    }
    for (const auto& b : g_bufs) {
        if (!b.isInput && b.buffers[index]) {
            std::memset(b.buffers[index], 0, static_cast<size_t>(g_bufSize) * sizeof(int32_t));
        }
    }
    if (g_driver) {
        g_driver->outputReady();
    }
}

void cbSampleRateDidChange(ASIOSampleRate sr) {
    std::printf("  [HOST] sampleRateDidChange: %.1f\n", sr);
}

long cbAsioMessage(long selector, long value, void*, double*) {
    if (selector == kAsioSelectorSupported) {
        return (value == kAsioEngineVersion || value == kAsioSupportsTimeInfo || value == kAsioResetRequest) ? 1 : 0;
    }
    if (selector == kAsioEngineVersion) return 2;
    if (selector == kAsioSupportsTimeInfo) return 1;
    if (selector == kAsioResetRequest) return 1;
    return 0;
}

ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool directProcess) {
    cbBufferSwitch(index, directProcess);
    return params;
}

void cbTapBufferSwitch(long index, ASIOBool) {
    g_tapCallbacks.fetch_add(1, std::memory_order_relaxed);
    if (g_tapBufs.size() >= 2) {
        updatePeakF32(g_tapPeakL, g_tapBufs[0].buffers[index], g_tapBufSize);
        updatePeakF32(g_tapPeakR, g_tapBufs[1].buffers[index], g_tapBufSize);
    }
}

ASIOTime* cbTapBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool directProcess) {
    cbTapBufferSwitch(index, directProcess);
    return params;
}

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
using DllCanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

bool testTapDriver(DllGetClassObjectFn getClassObject) {
    std::printf("\n=== Testing SplitASIO Tap (Path B: Shared-Memory ASIO Input) ===\n");

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_SplitAsioTap, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        std::printf("FAILED: DllGetClassObject(Tap) returned 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    IASIO* tapDrv = nullptr;
    hr = factory->CreateInstance(nullptr, CLSID_SplitAsioTap, reinterpret_cast<void**>(&tapDrv));
    factory->Release();
    if (FAILED(hr) || !tapDrv) {
        std::printf("FAILED: CreateInstance(Tap) returned 0x%08lx\n", static_cast<unsigned long>(hr));
        return false;
    }

    if (!tapDrv->init(GetDesktopWindow())) {
        std::printf("FAILED: Tap init() failed\n");
        tapDrv->Release();
        return false;
    }

    char name[64]{};
    tapDrv->getDriverName(name);
    long inCh = 0, outCh = 0;
    tapDrv->getChannels(&inCh, &outCh);
    std::printf("Loaded tap driver: '%s' (%ld in, %ld out)\n", name, inCh, outCh);

    g_tapBufSize = 128;
    g_tapBufs.clear();
    for (long c = 0; c < 2; ++c) {
        ASIOBufferInfo bi{};
        bi.isInput = ASIOTrue;
        bi.channelNum = c;
        g_tapBufs.push_back(bi);
    }

    ASIOCallbacks tapCbs{ cbTapBufferSwitch, cbSampleRateDidChange, cbAsioMessage, cbTapBufferSwitchTimeInfo };
    ASIOError err = tapDrv->createBuffers(g_tapBufs.data(), 2, g_tapBufSize, &tapCbs);
    if (err != ASE_OK) {
        std::printf("FAILED: Tap createBuffers returned %ld\n", err);
        tapDrv->Release();
        return false;
    }

    // Open a publisher and push a 1 kHz sine wave for ~250 ms
    SharedTapPublisher pub;
    bool pubOk = pub.open(48000.0, 128u);
    if (!pubOk) {
        std::printf("FAILED: SharedTapPublisher::open failed\n");
        tapDrv->disposeBuffers();
        tapDrv->Release();
        return false;
    }

    g_tapCallbacks.store(0);
    g_tapPeakL.store(0.0);
    g_tapPeakR.store(0.0);
    tapDrv->start();

    std::vector<float> sineL(128, 0.0f);
    std::vector<float> sineR(128, 0.0f);
    double phase = 0.0;
    constexpr double kTwoPi = 6.283185307179586;
    for (int block = 0; block < 80; ++block) {
        for (int i = 0; i < 128; ++i) {
            float v = 0.5f * static_cast<float>(std::sin(phase));
            sineL[static_cast<size_t>(i)] = v;
            sineR[static_cast<size_t>(i)] = -v;
            phase += kTwoPi * 1000.0 / 48000.0;
        }
        pub.push(sineL.data(), sineR.data(), 128);
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }

    tapDrv->stop();
    pub.close();
    tapDrv->disposeBuffers();
    tapDrv->Release();

    std::printf("Tap callbacks: %llu, peaks: L=%.4f, R=%.4f (expected ~0.5000)\n",
        g_tapCallbacks.load(), g_tapPeakL.load(), g_tapPeakR.load());

    return g_tapCallbacks.load() >= 20 && g_tapPeakL.load() > 0.45 && g_tapPeakR.load() > 0.45;
}

} // namespace

int main() {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    HMODULE dll = LoadLibraryW(L"split_asio.dll");
    if (!dll) {
        std::printf("FAILED: Could not load split_asio.dll (err=%lu)\n", GetLastError());
        CoUninitialize();
        return 1;
    }

    auto getClassObject = reinterpret_cast<DllGetClassObjectFn>(GetProcAddress(dll, "DllGetClassObject"));
    auto canUnloadNow = reinterpret_cast<DllCanUnloadNowFn>(GetProcAddress(dll, "DllCanUnloadNow"));
    if (!getClassObject || !canUnloadNow) {
        std::printf("FAILED: Missing COM exports in split_asio.dll\n");
        FreeLibrary(dll);
        CoUninitialize();
        return 1;
    }

    bool tapPassed = testTapDriver(getClassObject);

    std::printf("\n=== Testing Main SplitASIO Driver ===\n");
    bool mainPassed = false;
    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_AsioBridge, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (SUCCEEDED(hr) && factory) {
        IASIO* drv = nullptr;
        hr = factory->CreateInstance(nullptr, CLSID_AsioBridge, reinterpret_cast<void**>(&drv));
        factory->Release();
        if (SUCCEEDED(hr) && drv) {
            if (drv->init(GetDesktopWindow())) {
                char name[64]{};
                drv->getDriverName(name);
                std::printf("Loaded driver: '%s' (ver %ld)\n", name, drv->getDriverVersion());

                long inCh = 0, outCh = 0;
                drv->getChannels(&inCh, &outCh);
                std::printf("Channels: %ld in, %ld out\n", inCh, outCh);

                long minSz = 0, maxSz = 0, prefSz = 0, gran = 0;
                drv->getBufferSize(&minSz, &maxSz, &prefSz, &gran);
                std::printf("BufferSize: min=%ld, max=%ld, pref=%ld, gran=%ld\n", minSz, maxSz, prefSz, gran);

                ASIOSampleRate sr = 0;
                drv->getSampleRate(&sr);
                std::printf("SampleRate: %.1f Hz\n", sr);

                g_bufSize = prefSz;
                g_bufs.clear();
                for (long c = 0; c < std::min<long>(2, inCh); ++c) {
                    ASIOBufferInfo bi{};
                    bi.isInput = ASIOTrue;
                    bi.channelNum = c;
                    g_bufs.push_back(bi);
                }
                for (long c = 0; c < std::min<long>(2, outCh); ++c) {
                    ASIOBufferInfo bi{};
                    bi.isInput = ASIOFalse;
                    bi.channelNum = c;
                    g_bufs.push_back(bi);
                }

                ASIOCallbacks cbs{ cbBufferSwitch, cbSampleRateDidChange, cbAsioMessage, cbBufferSwitchTimeInfo };
                g_driver = drv;

                if (drv->createBuffers(g_bufs.data(), static_cast<long>(g_bufs.size()), g_bufSize, &cbs) == ASE_OK) {
                    long inLat = 0, outLat = 0;
                    drv->getLatencies(&inLat, &outLat);
                    std::printf("Latencies: in=%ld frames (%.2f ms), out=%ld frames (%.2f ms)\n",
                        inLat, inLat * 1000.0 / sr, outLat, outLat * 1000.0 / sr);

                    if (drv->start() == ASE_OK) {
                        std::printf("Streaming for 2.0 seconds...\n");
                        std::this_thread::sleep_for(std::chrono::seconds(2));
                        uint64_t count = g_callbacks.load();
                        double expected = (sr / static_cast<double>(g_bufSize)) * 2.0;
                        std::printf("Callbacks: %llu (expected ~%.0f)\n", count, expected);
                        std::printf("Input peaks: In1=%.5f, In2=%.5f\n", g_peakIn0.load(), g_peakIn1.load());
                        drv->stop();
                        mainPassed = (count > 100);
                    }
                    drv->disposeBuffers();
                }
                g_driver = nullptr;
            } else {
                char err[256]{};
                drv->getErrorMessage(err);
                std::printf("Note: Main driver init() skipped/failed (hardware may be busy): %s\n", err);
            }
            drv->Release();
        }
    }

    HRESULT unloadHr = canUnloadNow();
    std::printf("\nDllCanUnloadNow() -> %s\n", (unloadHr == S_OK) ? "S_OK (clean)" : "S_FALSE (leak!)");
    std::printf("Summary: TapTest=%s, MainHardwareTest=%s\n",
        tapPassed ? "PASS" : "FAIL",
        mainPassed ? "PASS" : "SKIPPED/FAIL");

    FreeLibrary(dll);
    CoUninitialize();
    return (unloadHr == S_OK && tapPassed) ? 0 : 1;
}
