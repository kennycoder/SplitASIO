#include "../asio_abi.h"
#include "../driver_guid.h"

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

void updatePeak(std::atomic<double>& peak, const void* buf, long frames) {
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

void cbBufferSwitch(long index, ASIOBool) {
    g_callbacks.fetch_add(1, std::memory_order_relaxed);
    if (g_bufs.size() >= 2) {
        updatePeak(g_peakIn0, g_bufs[0].buffers[index], g_bufSize);
        updatePeak(g_peakIn1, g_bufs[1].buffers[index], g_bufSize);
    }
    // Keep outputs silent during automated test
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
    if (selector == kAsioResetRequest) {
        std::printf("  [HOST] Reset request received\n");
        return 1;
    }
    return 0;
}

ASIOTime* cbBufferSwitchTimeInfo(ASIOTime* params, long index, ASIOBool directProcess) {
    cbBufferSwitch(index, directProcess);
    return params;
}

using DllGetClassObjectFn = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
using DllCanUnloadNowFn = HRESULT(STDAPICALLTYPE*)();

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

    IClassFactory* factory = nullptr;
    HRESULT hr = getClassObject(CLSID_AsioBridge, IID_IClassFactory, reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        std::printf("FAILED: DllGetClassObject returned 0x%08lx\n", static_cast<unsigned long>(hr));
        FreeLibrary(dll);
        CoUninitialize();
        return 1;
    }

    IASIO* drv = nullptr;
    hr = factory->CreateInstance(nullptr, CLSID_AsioBridge, reinterpret_cast<void**>(&drv));
    factory->Release();
    if (FAILED(hr) || !drv) {
        std::printf("FAILED: CreateInstance returned 0x%08lx\n", static_cast<unsigned long>(hr));
        FreeLibrary(dll);
        CoUninitialize();
        return 1;
    }

    if (!drv->init(GetDesktopWindow())) {
        char err[256]{};
        drv->getErrorMessage(err);
        std::printf("FAILED: init() failed: %s\n", err);
        drv->Release();
        FreeLibrary(dll);
        CoUninitialize();
        return 1;
    }

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
    // Request 2 inputs (Scarlett In 1 & In 2) + 2 outputs (FiiO Out 1 & Out 2)
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

    ASIOError err = drv->createBuffers(g_bufs.data(), static_cast<long>(g_bufs.size()), g_bufSize, &cbs);
    if (err != ASE_OK) {
        char msg[256]{};
        drv->getErrorMessage(msg);
        std::printf("FAILED: createBuffers returned %ld (%s)\n", err, msg);
        drv->Release();
        FreeLibrary(dll);
        CoUninitialize();
        return 1;
    }

    long inLat = 0, outLat = 0;
    drv->getLatencies(&inLat, &outLat);
    std::printf("Latencies: in=%ld frames (%.2f ms), out=%ld frames (%.2f ms)\n",
        inLat, inLat * 1000.0 / sr, outLat, outLat * 1000.0 / sr);

    err = drv->start();
    if (err != ASE_OK) {
        std::printf("FAILED: start() returned %ld\n", err);
    } else {
        std::printf("Streaming for 3.0 seconds...\n");
        std::this_thread::sleep_for(std::chrono::seconds(3));
        uint64_t count = g_callbacks.load();
        double expected = (sr / static_cast<double>(g_bufSize)) * 3.0;
        std::printf("Callbacks: %llu (expected ~%.0f)\n", count, expected);
        std::printf("Input peaks: In1=%.5f, In2=%.5f\n", g_peakIn0.load(), g_peakIn1.load());
        drv->stop();
    }

    drv->disposeBuffers();
    g_driver = nullptr;
    drv->Release();

    HRESULT unloadHr = canUnloadNow();
    std::printf("DllCanUnloadNow() -> %s\n", (unloadHr == S_OK) ? "S_OK (clean)" : "S_FALSE (leak!)");

    FreeLibrary(dll);
    CoUninitialize();
    return (unloadHr == S_OK && g_callbacks.load() > 100) ? 0 : 1;
}
